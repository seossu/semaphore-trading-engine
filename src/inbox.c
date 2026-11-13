#include "inbox.h"

#include <string.h>

#include "common.h"

int inbox_init(inbox_t *ib) {
    memset(ib, 0, sizeof *ib);
    if (bbuf_init(&ib->ack, INBOX_ACK_CAP, sizeof(ack_t)) ||
        bbuf_init(&ib->ctl, INBOX_CTL_CAP, sizeof(event_t)) ||
        bbuf_init(&ib->md, INBOX_MD_CAP, sizeof(quote_t)) || isem_init_private(&ib->items, 0))
        return -1;
    atomic_init(&ib->md_dropped, 0);
    atomic_init(&ib->ctl_dropped, 0);
    atomic_init(&ib->ack_overflow, 0);
    return 0;
}

void inbox_destroy(inbox_t *ib) {
    bbuf_destroy(&ib->ack);
    bbuf_destroy(&ib->ctl);
    bbuf_destroy(&ib->md);
    isem_close(&ib->items);
}

void inbox_put_ack(inbox_t *ib, const ack_t *a) {
    if (!bbuf_tryput(&ib->ack, a)) {
        /* 일어나면 안 되는 일: inflight 세마포 용량 논증이 깨졌다는 뜻 */
        atomic_fetch_add(&ib->ack_overflow, 1);
        ig_die("ACK 큐 overflow: inflight 불변식 위반 (cid=%llu)", (unsigned long long)a->cid);
    }
    isem_post(&ib->items);
}

void inbox_put_ctl(inbox_t *ib, const event_t *ev) {
    bbuf_put(&ib->ctl, ev);
    isem_post(&ib->items);
}

int inbox_tryput_ctl(inbox_t *ib, const event_t *ev) {
    if (!bbuf_tryput(&ib->ctl, ev)) {
        atomic_fetch_add(&ib->ctl_dropped, 1);
        return 0;
    }
    isem_post(&ib->items);
    return 1;
}

int inbox_offer_quote(inbox_t *ib, const quote_t *q) {
    if (!bbuf_tryput(&ib->md, q)) {
        atomic_fetch_add(&ib->md_dropped, 1);
        return 0;
    }
    isem_post(&ib->items);
    return 1;
}

void inbox_get(inbox_t *ib, event_t *out) {
    isem_wait(&ib->items);
    if (bbuf_tryget(&ib->ack, &out->u.a)) {
        out->type = EV_ACK;
        return;
    }
    if (bbuf_tryget(&ib->ctl, out)) return;
    if (bbuf_tryget(&ib->md, &out->u.q)) {
        out->type = EV_QUOTE;
        return;
    }
    ig_die("inbox: items>0인데 세 큐가 모두 비어 있음 (단일 소비자 가정 위반)");
}
