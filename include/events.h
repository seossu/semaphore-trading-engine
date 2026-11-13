/* 스레드/프로세스 사이를 오가는 메시지 타입. 전부 고정 크기 POD라 공유메모리 링버퍼에 memcpy로 넣을 수 있다. */
#ifndef IG_EVENTS_H
#define IG_EVENTS_H

#include <stdint.h>

#include "common.h"

#define QUOTE_SEQ_POISON UINT64_MAX

typedef struct {
    char sym[IG_SYM_LEN];
    double price;
    uint64_t seq;
    int64_t t_feed_ns; /* feeder가 응답 받은 시각 (CLOCK_MONOTONIC, 프로세스 공통 기준) */
    int64_t wall_ms;
} quote_t;

typedef enum { SIDE_BUY = 1, SIDE_SELL = -1 } side_t;

typedef struct {
    uint64_t cid; /* client order id (엔진 내부) */
    int strat;
    side_t side;
    int qty;
    double price; /* market이면 0 */
    int market;
    int is_exit;
    int poison; /* 게이트웨이 워커 종료 신호 */
    int64_t t_quote_ns, t_decide_ns;
} order_req_t;

typedef enum { ACK_RECEIVED = 0, ACK_REJECTED = 1, ACK_FILLED = 2 } ack_type_t;

typedef struct {
    ack_type_t type;
    uint64_t cid;
    int strat;
    side_t side;
    int qty;
    double price;
    int rate_limited;
    char odno[IG_ODNO_LEN]; /* 브로커 주문번호 */
    char msg[IG_MSG_LEN];
    int64_t t_quote_ns, t_decide_ns, t_ack_ns;
} ack_t;

typedef enum { EV_QUOTE, EV_ACK, EV_CLOCK, EV_FLATTEN, EV_SHUTDOWN } ev_type_t;

typedef struct {
    ev_type_t type;
    union {
        quote_t q;
        ack_t a;
        int64_t clock_ms;
    } u;
} event_t;

static inline const char *ack_name(ack_type_t t) {
    return t == ACK_RECEIVED ? "RECEIVED" : t == ACK_REJECTED ? "REJECTED" : "FILLED";
}

#endif
