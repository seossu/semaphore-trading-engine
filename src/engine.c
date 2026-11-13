/* 엔진 프로세스의 보조 스레드들과 시작/종료 순서.
 *
 * 종료 순서 (교착 상태가 없도록 "생산자 → 소비자" 방향으로 하나씩 닫는다):
 *   0) (main) feeder 프로세스 종료 → md 링에 poison → md_receiver 종료
 *   1) nav, clock 스레드 종료 (stop_aux)
 *   2) 전략마다 SHUTDOWN → 전략 스레드 join   (전략이 order_q에 막혀 있어도 워커가 아직 살아 있으므로 풀림)
 *   3) 워커 수만큼 poison 주문 → 워커 join    (워커가 rl_trade에 막혀 있어도 refill이 살아 있으므로 풀림)
 *   4) 거래소 poison → join                  (워커가 다 끝났으니 더 들어올 주문이 없음)
 *   5) refill 스레드 종료
 * 각 단계에서 "기다리는 쪽이 의존하는 상대"는 아직 살아 있다. 그래서 wait-for 그래프에 사이클이 생기지 않는다.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

typedef struct {
    engine_t *e;
    int idx;
} gw_arg_t;
static gw_arg_t g_gw_args[IG_MAX_WORKERS];

static double bits_to_d(long long b) {
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}
static long long d_to_bits(double d) {
    long long b;
    memcpy(&b, &d, sizeof b);
    return b;
}

double engine_last_px(engine_t *e) { return bits_to_d(atomic_load(&e->last_px_bits)); }

void engine_deliver_ack(engine_t *e, ack_t *a) {
    a->t_ack_ns = ig_now_ns();
    inbox_put_ack(&e->inbox[a->strat], a);
}

void engine_apply_and_deliver_fill(engine_t *e, ack_t *fill) {
    book_apply_fill(&e->book, fill->strat, fill->side, fill->qty, fill->price); /* 장부 먼저 */
    atomic_fetch_add(&e->acks_filled, 1);
    engine_deliver_ack(e, fill);
}

/* ---------- refill: 두 rate limiter의 만기된 permit을 V() ---------- */
static void *refill_thread(void *arg) {
    engine_t *e = arg;
    while (!atomic_load(&e->stop_refill)) {
        int64_t a = rl_refill(&e->rl_md);
        int64_t b = rl_refill(&e->rl_trade);
        int64_t w = a < b ? a : b;
        if (w > 5000000) w = 5000000;
        if (w < 200000) w = 200000;
        ig_sleep_ns(w);
    }
    return NULL;
}

/* ---------- md_receiver: 공유메모리 링 → 전략별 우편함으로 복제(fan-out) ---------- */
static void *md_thread(void *arg) {
    engine_t *e = arg;
    for (;;) {
        quote_t q;
        bbuf_get(&e->md_ring, &q);
        if (q.seq == QUOTE_SEQ_POISON) break;
        atomic_fetch_add(&e->quotes_in, 1);
        atomic_store(&e->last_px_bits, d_to_bits(q.price));
        /* 느린 전략 하나 때문에 다른 전략이 시세를 못 받는 일이 없도록, 우편함이 차 있으면 버린다 */
        for (int i = 0; i < e->cfg.nstrat; i++) inbox_offer_quote(&e->inbox[i], &q);
    }
    return NULL;
}

/* ---------- 게이트웨이 워커 ×M ---------- */
typedef struct {
    engine_t *e;
    ack_t *a;
} recv_cb_t;

static void push_received(void *arg) {
    recv_cb_t *c = arg;
    atomic_fetch_add(&c->e->acks_received, 1);
    engine_deliver_ack(c->e, c->a); /* registry 락 안에서 호출됨 (inbox.h 용량 논증상 안 막힘) */
}

static void *gateway_thread(void *arg) {
    gw_arg_t *ga = arg;
    engine_t *e = ga->e;
    int w = ga->idx;
    for (;;) {
        order_req_t r;
        bbuf_get(&e->order_q, &r);
        if (r.poison) break;
        samples_push(&e->gw_wait[w], ig_now_ns() - r.t_decide_ns);

        rl_acquire(&e->rl_trade); /* 초당 호출 한도 */
        char odno[IG_ODNO_LEN] = "", msg[IG_MSG_LEN] = "";
        int64_t t0 = ig_now_ns();
        int rc = e->cfg.exec_src == SRC_KIS ? kis_place(&e->kis, &r, odno, msg)
                                            : mock_exec_place(e, &r, odno, msg);
        rl_release_deferred(&e->rl_trade); /* 응답 시각 기준으로 1초 뒤 반납 예약 */
        samples_push(&e->gw_place[w], ig_now_ns() - t0);
        atomic_fetch_add(&e->orders_sent, 1);

        ack_t a;
        memset(&a, 0, sizeof a);
        a.cid = r.cid;
        a.strat = r.strat;
        a.side = r.side;
        a.qty = r.qty;
        a.price = r.price;
        a.t_quote_ns = r.t_quote_ns;
        a.t_decide_ns = r.t_decide_ns;

        if (rc != KIS_OK) {
            a.type = ACK_REJECTED;
            a.rate_limited = rc == KIS_RATE_LIMITED;
            snprintf(a.msg, sizeof a.msg, "%s", msg);
            if (a.rate_limited) atomic_fetch_add(&e->rl_rejected_trade, 1);
            if (rc == KIS_TOKEN_EXPIRED) atomic_store(&e->shm->tok.refresh_req, 1);
            atomic_fetch_add(&e->acks_rejected, 1);
            engine_deliver_ack(e, &a);
            continue;
        }

        a.type = ACK_RECEIVED;
        snprintf(a.odno, sizeof a.odno, "%s", odno);
        reg_entry_t ent;
        memset(&ent, 0, sizeof ent);
        snprintf(ent.odno, sizeof ent.odno, "%s", odno);
        ent.cid = r.cid;
        ent.strat = r.strat;
        ent.t_quote_ns = r.t_quote_ns;
        ent.t_decide_ns = r.t_decide_ns;
        recv_cb_t cb = {e, &a};
        ack_t parked[4];
        int n = reg_bind(&e->reg, &ent, push_received, &cb, parked, 4);
        for (int i = 0; i < n; i++) engine_apply_and_deliver_fill(e, &parked[i]);

        if (e->cfg.exec_src == SRC_KIS) {
            /* KIS 실시간 체결통보(WebSocket)는 미연동 → 접수 성공 시 주문가 체결로 간주
             * (실제 체결은 nav 폴러의 잔고로 확인) */
            ack_t f = a;
            f.type = ACK_FILLED;
            if (f.price <= 0) f.price = engine_last_px(e);
            if (reg_route(&e->reg, &f)) engine_apply_and_deliver_fill(e, &f);
        }
    }
    return NULL;
}

/* ---------- nav: 브로커 잔고 조회 → 장부와 대조 ---------- */
static void *nav_thread(void *arg) {
    engine_t *e = arg;
    while (!atomic_load(&e->stop_aux)) {
        ig_sleep_until_stop(e->cfg.nav_interval_ns, &e->stop_aux);
        if (atomic_load(&e->stop_aux)) break;
        rl_acquire(&e->rl_trade);
        balance_t b;
        char err[160] = "";
        int rc = e->cfg.exec_src == SRC_KIS ? kis_balance(&e->kis, &b, err, sizeof err)
                                            : mock_exec_balance(e, &b);
        rl_release_deferred(&e->rl_trade);
        if (rc != KIS_OK) {
            atomic_fetch_add(&e->nav_fail, 1);
            if (rc == KIS_RATE_LIMITED) atomic_fetch_add(&e->rl_rejected_trade, 1);
            if (rc == KIS_TOKEN_EXPIRED) atomic_store(&e->shm->tok.refresh_req, 1);
            if (e->cfg.verbose || rc != KIS_RATE_LIMITED) ig_log("nav", "잔고 조회 실패: %s", err);
            continue;
        }
        atomic_fetch_add(&e->nav_ok, 1);
        int mismatch = book_set_broker(&e->book, b.net_known, b.net, b.cash, b.pnl);
        /* 체결 직후에는 거래소 원장과 장부 반영 사이에 아주 짧은 틈이 있다 → 두 번 연속일 때만 경고 */
        e->nav_mismatch_streak = mismatch ? e->nav_mismatch_streak + 1 : 0;
        if (e->nav_mismatch_streak == 2) {
            atomic_fetch_add(&e->nav_mismatch_persistent, 1);
            book_view_t v;
            book_snapshot(&e->book, &v);
            ig_log("nav", "경고: 브로커 순포지션 %d ≠ 장부 %d (2회 연속)", b.net, v.net);
        }
    }
    return NULL;
}

/* ---------- clock: 1초 틱, 장마감 전량청산, 상태 출력, 토큰 갱신 ---------- */
static void broadcast_flatten(engine_t *e) {
    atomic_store(&e->halted, 1); /* 신규 진입 차단 */
    event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_FLATTEN;
    for (int i = 0; i < e->cfg.nstrat; i++) inbox_put_ctl(&e->inbox[i], &ev);
    atomic_fetch_add(&e->flatten_rounds, 1);
}

static void print_status(engine_t *e) {
    book_view_t v;
    book_snapshot(&e->book, &v);
    ig_log("status",
           "시세 %ld | 주문 %ld 접수 %ld 체결 %ld 거절 %ld (속도제한 %ld) | 순포지션 %d 실현 %.2fpt | "
           "서버 허용 %ld 거절 %ld",
           atomic_load(&e->quotes_in), atomic_load(&e->orders_sent), atomic_load(&e->acks_received),
           atomic_load(&e->acks_filled), atomic_load(&e->acks_rejected),
           atomic_load(&e->rl_rejected_trade) + atomic_load(&e->shm->poll_ratelimited), v.net,
           v.realized_pts, atomic_load(&e->shm->srv.admitted), atomic_load(&e->shm->srv.rejected));
}

static void *clock_thread(void *arg) {
    engine_t *e = arg;
    int64_t last_status = ig_now_ns(), last_auth_try = 0;
    int flatten_sent = 0, flat_logged = 0;
    while (!atomic_load(&e->stop_aux)) {
        ig_sleep_until_stop(1000000000LL, &e->stop_aux);
        if (atomic_load(&e->stop_aux)) break;
        event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = EV_CLOCK;
        ev.u.clock_ms = ig_wall_ms();
        for (int i = 0; i < e->cfg.nstrat; i++) inbox_tryput_ctl(&e->inbox[i], &ev);

        int64_t el_s = (ig_now_ns() - e->t_start_ns) / 1000000000LL;
        int due = e->cfg.duration_s > 0 ? el_s >= e->cfg.duration_s - e->cfg.flatten_before_end_s
                                        : ig_local_hhmmss() >= e->cfg.exit_hhmmss;
        if (due && !flatten_sent) {
            ig_log("clock", "장마감 전량청산 시작 (모든 전략에 FLATTEN, 신규 진입 차단)");
            e->t_flatten_ns = ig_now_ns();
            broadcast_flatten(e);
            flatten_sent = 1;
        }
        if (flatten_sent && !flat_logged) {
            book_view_t v;
            book_snapshot(&e->book, &v);
            if (v.net == 0) {
                ig_log("clock", "전량청산 확인: 순포지션 0 (%.0fms 소요)",
                       (ig_now_ns() - e->t_flatten_ns) / 1e6);
                atomic_store(&e->flatten_done, 1);
                flat_logged = 1;
            }
        }
        if (e->cfg.price_src == SRC_KIS || e->cfg.exec_src == SRC_KIS) {
            int64_t age = ig_wall_ms() - atomic_load(&e->shm->tok.issued_ms);
            int need = atomic_load(&e->shm->tok.refresh_req) || age > 20LL * 3600 * 1000;
            if (need && ig_now_ns() - last_auth_try > 65000000000LL) { /* tokenP 1분 1회 제한 */
                last_auth_try = ig_now_ns();
                char err[256];
                if (kis_authenticate(&e->kis, err, sizeof err) == KIS_OK) ig_log("clock", "KIS 토큰 갱신");
                else ig_log("clock", "KIS 토큰 갱신 실패: %s", err);
            }
        }
        if (ig_now_ns() - last_status >= 5000000000LL) {
            last_status = ig_now_ns();
            print_status(e);
        }
    }
    return NULL;
}

int engine_start_threads(engine_t *e) {
    if (pthread_create(&e->th_refill, NULL, refill_thread, e)) return -1;
    if (e->cfg.exec_src == SRC_MOCK && pthread_create(&e->th_exch, NULL, exchange_thread, e)) return -1;
    for (int i = 0; i < e->cfg.nworkers; i++) {
        g_gw_args[i].e = e;
        g_gw_args[i].idx = i;
        if (pthread_create(&e->th_gw[i], NULL, gateway_thread, &g_gw_args[i])) return -1;
    }
    for (int i = 0; i < e->cfg.nstrat; i++)
        if (pthread_create(&e->th_strat[i], NULL, strategy_thread, e->strat[i])) return -1;
    if (pthread_create(&e->th_md, NULL, md_thread, e)) return -1;
    if (pthread_create(&e->th_nav, NULL, nav_thread, e)) return -1;
    if (pthread_create(&e->th_clock, NULL, clock_thread, e)) return -1;
    return 0;
}

/* main이 feeder를 정리하고 md 링에 poison을 넣은 뒤 호출한다 */
void engine_shutdown(engine_t *e) {
    pthread_join(e->th_md, NULL);

    atomic_store(&e->stop_aux, 1);
    pthread_join(e->th_nav, NULL);
    pthread_join(e->th_clock, NULL);

    event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_SHUTDOWN;
    for (int i = 0; i < e->cfg.nstrat; i++) inbox_put_ctl(&e->inbox[i], &ev);
    for (int i = 0; i < e->cfg.nstrat; i++) pthread_join(e->th_strat[i], NULL);

    order_req_t poison;
    memset(&poison, 0, sizeof poison);
    poison.poison = 1;
    for (int i = 0; i < e->cfg.nworkers; i++) bbuf_put(&e->order_q, &poison);
    for (int i = 0; i < e->cfg.nworkers; i++) pthread_join(e->th_gw[i], NULL);

    if (e->cfg.exec_src == SRC_MOCK) {
        exch_order_t xp;
        memset(&xp, 0, sizeof xp);
        xp.poison = 1;
        bbuf_put(&e->exch_q, &xp);
        pthread_join(e->th_exch, NULL);
    }

    atomic_store(&e->stop_refill, 1);
    pthread_join(e->th_refill, NULL);
}
