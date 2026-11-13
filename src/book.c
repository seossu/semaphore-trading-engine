#include "book.h"

#include <stdlib.h>
#include <string.h>

int book_init(book_t *b, int nstrat) {
    memset(b, 0, sizeof *b);
    b->v.nstrat = nstrat;
    return rw_init(&b->lock);
}

void book_destroy(book_t *b) { rw_destroy(&b->lock); }

void book_apply_fill(book_t *b, int strat, side_t side, int qty, double px) {
    rw_wrlock(&b->lock);
    pos_t *p = &b->v.pos[strat];
    int dq = (int)side * qty;
    if (p->qty == 0 || (p->qty > 0) == (dq > 0)) {
        /* 같은 방향으로 늘리기: 평단 갱신 */
        int aq = abs(p->qty);
        p->avg_px = (p->avg_px * aq + px * qty) / (aq + qty);
        p->qty += dq;
    } else {
        /* 반대 방향: 줄이는 만큼 실현손익 */
        int closed = abs(p->qty) < qty ? abs(p->qty) : qty;
        double pnl = closed * (px - p->avg_px) * (p->qty > 0 ? 1 : -1);
        p->realized_pts += pnl;
        b->v.realized_pts += pnl;
        int before = p->qty;
        p->qty += dq;
        if (p->qty == 0) p->avg_px = 0;
        else if ((before > 0) != (p->qty > 0)) p->avg_px = px; /* 반대로 뒤집힘 */
    }
    p->fills++;
    b->v.fills++;
    b->v.net += dq;
    rw_wrunlock(&b->lock);
}

void book_snapshot(book_t *b, book_view_t *out) {
    rw_rdlock(&b->lock);
    *out = b->v;
    rw_rdunlock(&b->lock);
}

int book_set_broker(book_t *b, int net_known, int net, double cash, double pnl) {
    rw_wrlock(&b->lock);
    b->v.broker_net_known = net_known;
    b->v.broker_net = net;
    b->v.broker_cash = cash;
    b->v.broker_pnl = pnl;
    b->v.broker_ts_ms = ig_wall_ms();
    int mismatch = net_known && net != b->v.net;
    rw_wrunlock(&b->lock);
    return mismatch;
}
