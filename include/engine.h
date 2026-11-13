/* 엔진 전체 구조체와 설정. 스레드/프로세스 구성은 docs/DESIGN.md 그림 참고.
 *
 *   [feeder 프로세스] --(공유메모리 SPSC 링, named sem)--> md_receiver 스레드
 *        └ 시세 조회 permit(rl_md, named sem)을 엔진의 refill 스레드와 공유
 *   md_receiver --(전략별 inbox.md, 꽉 차면 버림)--> 전략 스레드 ×N
 *   전략 스레드 --(order_q, MPMC 유한 버퍼)--> 게이트웨이 워커 ×M --(rl_trade)--> 브로커
 *   브로커 체결 --(registry로 주인 찾기)--> 장부(book, RW락) 반영 --> 전략 inbox.ack
 *   clock 스레드: 1초 틱, 장마감 전량청산, 상태 출력 / nav 스레드: 잔고 조회 후 장부 대조
 */
#ifndef IG_ENGINE_H
#define IG_ENGINE_H

#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>

#include "bbuf.h"
#include "book.h"
#include "inbox.h"
#include "kis.h"
#include "mock_server.h"
#include "ratelimit.h"
#include "registry.h"

#define MD_RING_CAP 1024
#define SHM_MAGIC 0x49474E43u /* "IGNC" */

enum { SRC_MOCK = 0, SRC_KIS = 1 };

typedef struct {
    int price_src; /* 시세: mock / kis */
    int exec_src;  /* 주문: mock / kis */
    const char *mode_name;
    int duration_s; /* 0이면 Ctrl+C까지 */
    int nstrat, nworkers;
    int rl_enabled, rl_md_cap, rl_trade_cap;
    int64_t rl_hold_ns;
    int server_limit;
    int64_t feed_min_interval_ns, nav_interval_ns;
    int64_t bar_ms;
    double loss_pct, tp_pct;
    int qty, max_net, inflight_cap;
    int exit_hhmmss;
    int flatten_before_end_s;
    double fast_fill_prob, reject_prob;
    uint64_t seed;
    char inst[IG_SYM_LEN];
    char env_path[512];
    char out_dir[512];
    int verbose;
} config_t;

/* 엔진과 feeder 프로세스가 같이 보는 공유메모리 (shm_open + mmap). 데이터만 두고,
 * 세마포 핸들(sem_t*)은 프로세스마다 이름으로 따로 연다. */
typedef struct {
    uint32_t magic;
    char ipc_prefix[IG_IPC_PREFIX_LEN];
    int price_src;
    char inst[IG_SYM_LEN];
    int64_t feed_min_interval_ns;
    uint64_t seed;
    atomic_int stop;
    atomic_llong last_px_bits; /* feeder 재시작 후 가격 이어가기 */
    atomic_long polls, poll_err, poll_ratelimited, ring_dropped, feeder_starts;
    mock_server_t srv;
    rl_state_t rl_md;
    token_state_t tok;
    _Alignas(64) unsigned char ring_mem[];
} shm_t;

typedef struct strategy strategy_t;

typedef struct {
    char odno[IG_ODNO_LEN];
    side_t side;
    int qty;
    double price;
    int64_t fill_at_ns;
    int poison;
} exch_order_t;

typedef struct engine {
    config_t cfg;

    /* IPC */
    char shm_name[32];
    size_t shm_size;
    shm_t *shm;
    bbuf_t md_ring;
    ratelimit_t rl_md;
    isem_t tok_ready;
    pid_t feeder_pid;
    char exe_path[1024];

    /* 프로세스 내부 동기화 */
    ratelimit_t rl_trade;
    bbuf_t order_q;
    bbuf_t exch_q;
    registry_t reg;
    book_t book;
    isem_t exposure; /* 계좌 전체 롱 한도(계약 수) */
    isem_t csv_mtx;
    FILE *trades_csv;
    inbox_t inbox[IG_MAX_STRAT];
    strategy_t *strat[IG_MAX_STRAT];

    /* 모의 거래소 원장 (브로커 쪽 장부. 내부 장부와 독립 → NAV 대조가 의미 있음) */
    isem_t ledger_mtx;
    int ledger_net;
    double ledger_cash;
    atomic_ullong next_odno;

    kis_t kis;

    atomic_int stop_aux, stop_refill, halted, flatten_done;
    atomic_ullong next_cid;
    atomic_llong last_px_bits;
    int64_t t_start_ns, t_flatten_ns;

    atomic_long quotes_in, orders_sent, acks_received, acks_rejected, acks_filled;
    atomic_long rl_rejected_trade, nav_ok, nav_fail, nav_mismatch_persistent, flatten_rounds;
    int nav_mismatch_streak;

    samples_t gw_wait[IG_MAX_WORKERS], gw_place[IG_MAX_WORKERS];

    pthread_t th_refill, th_md, th_nav, th_clock, th_exch;
    pthread_t th_gw[IG_MAX_WORKERS], th_strat[IG_MAX_STRAT];
} engine_t;

/* engine.c */
int engine_start_threads(engine_t *e);
void engine_shutdown(engine_t *e);
void engine_deliver_ack(engine_t *e, ack_t *a);
void engine_apply_and_deliver_fill(engine_t *e, ack_t *fill);
double engine_last_px(engine_t *e);

/* exchange.c (모의 주문 실행) */
int mock_exec_place(engine_t *e, const order_req_t *r, char *odno, char *msg);
int mock_exec_balance(engine_t *e, balance_t *b);
void *exchange_thread(void *arg);

/* strategy.c */
strategy_t *strategy_new(engine_t *e, int id, int window);
void strategy_free(strategy_t *s);
void *strategy_thread(void *arg);
void strategy_report(strategy_t *s, FILE *out);
int strategy_inflight_free(strategy_t *s);
int strategy_holding(strategy_t *s);
void strategy_collect_latency(strategy_t *s, samples_t *mdq, samples_t *e2e_recv, samples_t *e2e_fill);
long strategy_counter(strategy_t *s, int which);
enum { SC_QUOTES, SC_ENTRIES, SC_EXITS, SC_REJECTS, SC_RISK_BLOCKED, SC_INFLIGHT_BLOCKED, SC_STALE_ACK };

/* feeder.c */
int feeder_main(const char *shm_name);

#endif
