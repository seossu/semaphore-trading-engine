/* 포지션 장부: 전략별 보유 수량/평단/실현손익 + 브로커가 알려준 잔고.
 * Readers-Writers 락(rwlock.h)으로 보호한다.
 *   writer: 체결 반영(게이트웨이 워커, 거래소 스레드), 잔고 폴러
 *   reader: 상태 모니터, 전량청산 확인, 종료 시 검증
 */
#ifndef IG_BOOK_H
#define IG_BOOK_H

#include "common.h"
#include "events.h"
#include "rwlock.h"

typedef struct {
    int qty;
    double avg_px;
    double realized_pts;
    long fills;
} pos_t;

typedef struct {
    int nstrat;
    pos_t pos[IG_MAX_STRAT];
    int net;
    long fills;
    double realized_pts;
    int broker_net, broker_net_known;
    double broker_cash, broker_pnl;
    int64_t broker_ts_ms;
} book_view_t;

typedef struct {
    rwlock_t lock;
    book_view_t v;
} book_t;

int book_init(book_t *b, int nstrat);
void book_destroy(book_t *b);
void book_apply_fill(book_t *b, int strat, side_t side, int qty, double px);
void book_snapshot(book_t *b, book_view_t *out);
/* 브로커 잔고 반영. 내부 장부와 순포지션이 다르면 1 */
int book_set_broker(book_t *b, int net_known, int net, double cash, double pnl);

#endif
