/* N봉 고가 돌파 롱온리 전략.
 *
 * 전략 하나 = 스레드 하나. 자기 상태는 자기 스레드만 만지므로 전략 내부에는 락이 없다.
 * 다른 스레드와 닿는 지점은 전부 세마포다.
 *   inbox(우편함)  : 이벤트 수신
 *   order_q        : 주문 전송 (꽉 차면 잠든다 = backpressure)
 *   exposure       : 계좌 전체 롱 한도. 진입 직전에 trywait로 "예약"한다
 *   inflight       : 이 전략의 미체결 주문 수 한도(K). ACK 큐 용량 증명의 근거
 *
 * exposure를 RW락으로 "읽고 판단"하지 않고 세마포 permit으로 "예약"하는 이유:
 *   전략 A와 B가 동시에 장부를 읽고 "net=1 < 한도 2, 진입 가능"이라고 판단하면 둘 다 진입해서 net=3이 된다.
 *   확인한 시점과 실행하는 시점 사이에 상태가 바뀌는 TOCTOU 문제다.
 *   P(exposure)는 "확인 + 차감"이 원자적이라 이 경쟁이 구조적으로 불가능하다.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

enum { ST_FLAT, ST_PENDING_ENTRY, ST_LONG, ST_PENDING_EXIT };
static const char *st_name[] = {"FLAT", "PENDING_ENTRY", "LONG", "PENDING_EXIT"};

#define MAX_WINDOW 64

struct strategy {
    engine_t *e;
    int id;
    char name[24];
    int window;
    isem_t inflight;

    int state, halted;
    int64_t bar_key;
    double bar_high;
    double highs[MAX_WINDOW];
    int nh, hpos;
    int64_t cooldown_until_bar;
    double entry_px, last_px;
    int64_t last_quote_ns;
    int holding_exposure; /* 지금 쥐고 있는 exposure permit 수 */

    uint64_t pending_cid;
    const char *pending_reason;

    long c[7];
    samples_t lat_mdq, lat_recv, lat_fill;
};

strategy_t *strategy_new(engine_t *e, int id, int window) {
    strategy_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->e = e;
    s->id = id;
    s->window = window < 1 ? 1 : (window > MAX_WINDOW ? MAX_WINDOW : window);
    snprintf(s->name, sizeof s->name, "BO%02d_w%d", id, s->window);
    s->bar_key = -1;
    if (isem_init_private(&s->inflight, (unsigned)e->cfg.inflight_cap)) {
        free(s);
        return NULL;
    }
    samples_init(&s->lat_mdq, 200000);
    samples_init(&s->lat_recv, 20000);
    samples_init(&s->lat_fill, 20000);
    return s;
}

void strategy_free(strategy_t *s) {
    if (!s) return;
    isem_close(&s->inflight);
    samples_free(&s->lat_mdq);
    samples_free(&s->lat_recv);
    samples_free(&s->lat_fill);
    free(s);
}

static void log_trade(strategy_t *s, const ack_t *a, const char *reason) {
    engine_t *e = s->e;
    if (!e->trades_csv) return;
    /* 여러 전략 스레드가 같은 파일에 쓴다 → 한 행을 원자적으로 쓰도록 이진 세마포로 감싼다 */
    isem_wait(&e->csv_mtx);
    fprintf(e->trades_csv, "%" PRId64 ",%s,%s,%d,%.2f,%s,%s,%.1f\n", ig_wall_ms(), s->name,
            a->side == SIDE_BUY ? "BUY" : "SELL", a->qty, a->price, reason ? reason : "", a->odno,
            (a->t_ack_ns - a->t_quote_ns) / 1000.0);
    isem_post(&e->csv_mtx);
}

/* 주문 전송. inflight permit을 못 얻으면 보내지 않는다. */
static int send_order(strategy_t *s, side_t side, int market, const char *reason, int is_exit) {
    engine_t *e = s->e;
    if (!isem_trywait(&s->inflight)) {
        s->c[SC_INFLIGHT_BLOCKED]++;
        return 0;
    }
    order_req_t r;
    memset(&r, 0, sizeof r);
    r.cid = atomic_fetch_add(&e->next_cid, 1) + 1;
    r.strat = s->id;
    r.side = side;
    r.qty = e->cfg.qty;
    r.market = market;
    r.price = market ? 0 : s->last_px;
    r.is_exit = is_exit;
    r.t_quote_ns = s->last_quote_ns;
    r.t_decide_ns = ig_now_ns();
    s->pending_cid = r.cid;
    s->pending_reason = reason;
    bbuf_put(&e->order_q, &r); /* 게이트웨이가 밀리면 여기서 잠든다 */
    if (e->cfg.verbose)
        ig_log(s->name, "%s %s @ %.2f (cid=%llu)", reason, side == SIDE_BUY ? "BUY" : "SELL",
               r.price, (unsigned long long)r.cid);
    return 1;
}

static void try_exit(strategy_t *s, const char *reason) {
    if (s->state != ST_LONG) return;
    /* 청산은 리스크를 줄이는 방향이라 exposure를 따로 얻을 필요가 없다. 할당은 체결 후 반납한다. */
    int market = s->e->cfg.exec_src == SRC_KIS; /* 실계좌 청산은 시장가로 확실히 */
    if (send_order(s, SIDE_SELL, market, reason, 1)) {
        s->state = ST_PENDING_EXIT;
        s->c[SC_EXITS]++;
    }
}

static void try_entry(strategy_t *s, double rolling_high) {
    engine_t *e = s->e;
    if (s->halted || atomic_load(&e->halted)) return;
    if (s->bar_key < s->cooldown_until_bar) return;
    /* ① 계좌 한도 예약 (원자적) */
    if (!isem_trywait(&e->exposure)) {
        s->c[SC_RISK_BLOCKED]++;
        return;
    }
    if (send_order(s, SIDE_BUY, 0, "BREAKOUT", 0)) {
        s->holding_exposure++;
        s->state = ST_PENDING_ENTRY;
        s->c[SC_ENTRIES]++;
        if (e->cfg.verbose)
            ig_log(s->name, "최근 %d봉 고가 %.2f 돌파 → 진입 주문", s->window, rolling_high);
    } else {
        isem_post(&e->exposure); /* ② 주문을 못 냈으면 예약을 돌려준다 */
    }
}

static void close_bar_if_needed(strategy_t *s, int64_t wall_ms) {
    int64_t key = wall_ms / s->e->cfg.bar_ms;
    if (s->bar_key < 0) {
        s->bar_key = key;
        s->bar_high = s->last_px;
        return;
    }
    if (key == s->bar_key) return;
    if (s->bar_high > 0) {
        s->highs[s->hpos] = s->bar_high;
        s->hpos = (s->hpos + 1) % s->window;
        if (s->nh < s->window) s->nh++;
    }
    s->bar_key = key;
    s->bar_high = 0;
}

static void on_quote(strategy_t *s, const quote_t *q) {
    s->c[SC_QUOTES]++;
    samples_push(&s->lat_mdq, ig_now_ns() - q->t_feed_ns);
    s->last_px = q->price;
    s->last_quote_ns = q->t_feed_ns;
    close_bar_if_needed(s, q->wall_ms);
    if (q->price > s->bar_high) s->bar_high = q->price;

    if (s->state == ST_FLAT && s->nh == s->window) {
        double hi = s->highs[0];
        for (int i = 1; i < s->nh; i++)
            if (s->highs[i] > hi) hi = s->highs[i];
        if (q->price > hi) try_entry(s, hi);
    } else if (s->state == ST_LONG) {
        const config_t *c = &s->e->cfg;
        if (s->halted) try_exit(s, "FLATTEN_RETRY");
        else if (q->price <= s->entry_px * (1.0 - c->loss_pct / 100.0)) try_exit(s, "LOSS_CUT");
        else if (q->price >= s->entry_px * (1.0 + c->tp_pct / 100.0)) try_exit(s, "TAKE_PROFIT");
    }
}

static void on_ack(strategy_t *s, const ack_t *a) {
    engine_t *e = s->e;
    if (a->type == ACK_RECEIVED) {
        samples_push(&s->lat_recv, a->t_ack_ns - a->t_quote_ns);
        return;
    }
    if (a->cid != s->pending_cid) {
        s->c[SC_STALE_ACK]++;
        ig_log(s->name, "예상 밖 ACK %s cid=%llu (pending=%llu)", ack_name(a->type),
               (unsigned long long)a->cid, (unsigned long long)s->pending_cid);
        isem_post(&s->inflight);
        return;
    }
    /* 최종 ACK(체결/거절)을 소비했으니 inflight permit 반납 → ACK 큐 용량 논증의 핵심 지점 */
    isem_post(&s->inflight);
    s->pending_cid = 0;

    if (a->type == ACK_REJECTED) {
        s->c[SC_REJECTS]++;
        if (s->state == ST_PENDING_ENTRY) {
            isem_post(&e->exposure);
            s->holding_exposure--;
            s->state = ST_FLAT;
        } else if (s->state == ST_PENDING_EXIT) {
            s->state = ST_LONG; /* 다음 틱에 다시 청산 시도 */
        }
        if (e->cfg.verbose || !a->rate_limited)
            ig_log(s->name, "거절: %s", a->msg);
        return;
    }

    /* FILLED */
    samples_push(&s->lat_fill, a->t_ack_ns - a->t_quote_ns);
    if (s->state == ST_PENDING_ENTRY) {
        s->state = ST_LONG;
        s->entry_px = a->price;
        log_trade(s, a, s->pending_reason);
        if (s->halted) try_exit(s, "FLATTEN"); /* 청산 지시 이후에 체결된 진입 → 즉시 청산 */
    } else if (s->state == ST_PENDING_EXIT) {
        s->state = ST_FLAT;
        isem_post(&e->exposure);
        s->holding_exposure--;
        s->cooldown_until_bar = s->bar_key + 1;
        log_trade(s, a, s->pending_reason);
    }
}

void *strategy_thread(void *arg) {
    strategy_t *s = arg;
    engine_t *e = s->e;
    inbox_t *ib = &e->inbox[s->id];
    for (;;) {
        event_t ev;
        inbox_get(ib, &ev);
        switch (ev.type) {
        case EV_QUOTE: on_quote(s, &ev.u.q); break;
        case EV_ACK: on_ack(s, &ev.u.a); break;
        case EV_CLOCK: close_bar_if_needed(s, ev.u.clock_ms); break;
        case EV_FLATTEN:
            s->halted = 1;
            try_exit(s, "FLATTEN");
            break;
        case EV_SHUTDOWN:
            if (e->cfg.verbose) ig_log(s->name, "종료 (state=%s)", st_name[s->state]);
            return NULL;
        }
    }
}

int strategy_inflight_free(strategy_t *s) { return isem_count_slow(&s->inflight); }
int strategy_holding(strategy_t *s) { return s->holding_exposure; }
long strategy_counter(strategy_t *s, int which) { return s->c[which]; }

void strategy_collect_latency(strategy_t *s, samples_t *mdq, samples_t *recv, samples_t *fill) {
    samples_merge(mdq, &s->lat_mdq);
    samples_merge(recv, &s->lat_recv);
    samples_merge(fill, &s->lat_fill);
}

void strategy_report(strategy_t *s, FILE *out) {
    fprintf(out, "  %-10s state=%-13s 시세=%-6ld 진입=%-4ld 청산=%-4ld 거절=%-4ld 한도차단=%-5ld", s->name,
            st_name[s->state], s->c[SC_QUOTES], s->c[SC_ENTRIES], s->c[SC_EXITS], s->c[SC_REJECTS],
            s->c[SC_RISK_BLOCKED]);
}
