/* 모의 주문 실행: 모의 KIS 서버(초당 제한) + 지연 + 거절 + 비동기 체결.
 *
 * place()는 게이트웨이 워커 스레드에서 호출된다. 주문을 받으면 exch_q에 넣고 ODNO를 바로 돌려준다.
 * 체결은 exchange_thread가 20~120ms 뒤에 비동기로 만든다(실제 거래소처럼 접수와 체결이 분리).
 * --fast-fill 비율만큼은 지연 0으로 체결시켜서, 워커가 registry에 등록하기 전에 체결이 도착하는
 * 경쟁 상황을 일부러 만든다(registry.h의 park 경로 검증).
 */
#include <stdio.h>
#include <string.h>

#include "engine.h"

static _Thread_local uint64_t t_rng;

static uint64_t *rng(engine_t *e) {
    if (!t_rng) t_rng = e->cfg.seed ^ (uint64_t)(uintptr_t)&t_rng ^ (uint64_t)ig_now_ns();
    return &t_rng;
}

static void net_latency(engine_t *e, int lo_ms, int hi_ms) {
    ig_sleep_ns((int64_t)((lo_ms + ig_rand_unit(rng(e)) * (hi_ms - lo_ms)) * 1e6));
}

int mock_exec_place(engine_t *e, const order_req_t *r, char *odno, char *msg) {
    if (!mock_server_admit(&e->shm->srv)) {
        net_latency(e, 2, 6);
        snprintf(msg, IG_MSG_LEN, "EGW00201 초당 거래건수를 초과하였습니다.(mock)");
        return KIS_RATE_LIMITED;
    }
    net_latency(e, 4, 15); /* 요청이 거래소까지 가는 시간 */
    if (ig_rand_unit(rng(e)) < e->cfg.reject_prob) {
        snprintf(msg, IG_MSG_LEN, "주문가능수량 부족(mock)");
        return KIS_ERR;
    }
    unsigned long long n = atomic_fetch_add(&e->next_odno, 1) + 1;
    snprintf(odno, IG_ODNO_LEN, "M%010llu", n);
    exch_order_t o;
    memset(&o, 0, sizeof o);
    snprintf(o.odno, sizeof o.odno, "%s", odno);
    o.side = r->side;
    o.qty = r->qty;
    o.price = r->market ? 0 : r->price;
    int fast = ig_rand_unit(rng(e)) < e->cfg.fast_fill_prob;
    o.fill_at_ns = ig_now_ns() + (fast ? 0 : (int64_t)((20 + ig_rand_unit(rng(e)) * 100) * 1e6));
    bbuf_put(&e->exch_q, &o); /* 거래소는 이 순간부터 체결을 만들 수 있다 */
    net_latency(e, 4, 15);    /* 응답(ODNO)이 돌아오는 시간 — 이 사이에 체결이 먼저 생길 수 있다 */
    return KIS_OK;
}

int mock_exec_balance(engine_t *e, balance_t *b) {
    memset(b, 0, sizeof *b);
    if (!mock_server_admit(&e->shm->srv)) return KIS_RATE_LIMITED;
    net_latency(e, 8, 30);
    isem_wait(&e->ledger_mtx);
    b->net = e->ledger_net;
    b->cash = e->ledger_cash;
    isem_post(&e->ledger_mtx);
    b->net_known = 1;
    b->ok = 1;
    return KIS_OK;
}

void *exchange_thread(void *arg) {
    engine_t *e = arg;
    for (;;) {
        exch_order_t o;
        bbuf_get(&e->exch_q, &o);
        if (o.poison) break;
        int64_t wait = o.fill_at_ns - ig_now_ns();
        if (wait > 0) ig_sleep_ns(wait);
        double px = o.price > 0 ? o.price : engine_last_px(e);

        isem_wait(&e->ledger_mtx);
        e->ledger_net += (int)o.side * o.qty;
        e->ledger_cash -= (double)o.side * o.qty * px * IG_KRW_PER_POINT;
        isem_post(&e->ledger_mtx);

        ack_t a;
        memset(&a, 0, sizeof a);
        a.type = ACK_FILLED;
        snprintf(a.odno, sizeof a.odno, "%s", o.odno);
        a.side = o.side;
        a.qty = o.qty;
        a.price = px;
        if (reg_route(&e->reg, &a)) engine_apply_and_deliver_fill(e, &a);
        /* else: 아직 등록 전 → park됨. 워커가 bind할 때 넘겨받아 전달한다 */
    }
    return NULL;
}
