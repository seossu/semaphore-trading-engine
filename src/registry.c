#include "registry.h"

#include <string.h>

#include "common.h"

static size_t hash_str(const char *s) {
    size_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

int reg_init(registry_t *r) {
    memset(r, 0, sizeof *r);
    return isem_init_private(&r->mtx, 1);
}

void reg_destroy(registry_t *r) { isem_close(&r->mtx); }

/* 아래 두 함수는 mtx를 쥔 상태에서만 호출 */
static reg_entry_t *find(registry_t *r, const char *odno) {
    size_t h = hash_str(odno);
    for (size_t i = 0; i < REG_CAP; i++) {
        reg_entry_t *e = &r->e[(h + i) % REG_CAP];
        if (e->used == 0) return NULL;
        if (e->used == 1 && strcmp(e->odno, odno) == 0) return e;
    }
    return NULL;
}

static reg_entry_t *free_slot(registry_t *r, const char *odno) {
    size_t h = hash_str(odno);
    for (size_t i = 0; i < REG_CAP; i++) {
        reg_entry_t *e = &r->e[(h + i) % REG_CAP];
        if (e->used != 1) return e;
    }
    return NULL;
}

static void fill_from(ack_t *a, const reg_entry_t *e) {
    a->cid = e->cid;
    a->strat = e->strat;
    a->t_quote_ns = e->t_quote_ns;
    a->t_decide_ns = e->t_decide_ns;
}

int reg_bind(registry_t *r, const reg_entry_t *ent, void (*in_lock)(void *), void *arg, ack_t *out,
             int outcap) {
    isem_wait(&r->mtx);
    reg_entry_t *slot = free_slot(r, ent->odno);
    if (!slot) ig_die("registry full");
    *slot = *ent;
    slot->used = 1;
    r->live++;
    if (in_lock) in_lock(arg); /* RECEIVED를 체결보다 먼저 우편함에 넣는다 */

    int n = 0;
    for (int i = 0; i < r->npark;) {
        if (strcmp(r->park[i].odno, ent->odno) == 0 && n < outcap) {
            out[n] = r->park[i];
            fill_from(&out[n], ent);
            n++;
            r->park[i] = r->park[--r->npark];
        } else {
            i++;
        }
    }
    if (n > 0) {
        slot->used = 2; /* 체결(최종 상태)까지 끝났으니 더 보관할 필요 없음 */
        r->live--;
    }
    isem_post(&r->mtx);
    return n;
}

int reg_route(registry_t *r, ack_t *fill) {
    isem_wait(&r->mtx);
    reg_entry_t *e = find(r, fill->odno);
    if (e) {
        fill_from(fill, e);
        e->used = 2;
        r->live--;
        isem_post(&r->mtx);
        return 1;
    }
    if (r->npark < REG_PARK) {
        r->park[r->npark++] = *fill;
    } else {
        /* 미체결 주문 수는 (전략 수 × inflight) ≤ 32로 묶여 있어 여기 올 수 없다 → 오면 불변식 위반 */
        r->park_overflow++;
        ig_die("registry park overflow (odno=%s)", fill->odno);
    }
    r->parked_total++;
    isem_post(&r->mtx);
    return 0;
}

int reg_parked_now(registry_t *r) {
    isem_wait(&r->mtx);
    int n = r->npark;
    isem_post(&r->mtx);
    return n;
}
