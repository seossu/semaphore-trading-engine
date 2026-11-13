#include "ratelimit.h"

#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void st_init(rl_state_t *st, int cap, int64_t hold_ns, int enabled) {
    st->cap = cap;
    st->enabled = enabled;
    st->hold_ns = hold_ns;
    for (int i = 0; i < RL_MAX_PERMITS; i++) atomic_init(&st->deadline[i], 0);
    atomic_init(&st->acquired, 0);
    atomic_init(&st->held, 0);
}

int rl_init(ratelimit_t *rl, int cap, int64_t hold_ns, int enabled) {
    memset(rl, 0, sizeof *rl);
    if (cap < 1 || cap > RL_MAX_PERMITS) return -1;
    rl->st = calloc(1, sizeof *rl->st);
    if (!rl->st) return -1;
    rl->own_state = 1;
    st_init(rl->st, cap, hold_ns, enabled);
    return isem_init_private(&rl->permits, (unsigned)cap);
}

int rl_create_shared(ratelimit_t *rl, rl_state_t *mem, int cap, int64_t hold_ns, int enabled,
                     const char *sem_name) {
    memset(rl, 0, sizeof *rl);
    if (cap < 1 || cap > RL_MAX_PERMITS) return -1;
    rl->st = mem;
    st_init(mem, cap, hold_ns, enabled);
    return isem_create_named(&rl->permits, sem_name, (unsigned)cap);
}

int rl_attach_shared(ratelimit_t *rl, rl_state_t *mem, const char *sem_name) {
    memset(rl, 0, sizeof *rl);
    rl->st = mem;
    return isem_open_named(&rl->permits, sem_name);
}

void rl_destroy(ratelimit_t *rl) {
    isem_close(&rl->permits);
    if (rl->own_state) free(rl->st);
    rl->st = NULL;
}

int rl_acquire_intr(ratelimit_t *rl, const atomic_int *stop) {
    atomic_fetch_add(&rl->st->acquired, 1);
    if (!rl->st->enabled) return 0;
    if (isem_wait_intr(&rl->permits, stop)) {
        atomic_fetch_sub(&rl->st->acquired, 1);
        return -1;
    }
    atomic_fetch_add(&rl->st->held, 1);
    return 0;
}

void rl_acquire(ratelimit_t *rl) { (void)rl_acquire_intr(rl, NULL); }

void rl_release_deferred(ratelimit_t *rl) {
    rl_state_t *st = rl->st;
    if (!st->enabled) return;
    long long t = ig_now_ns() + st->hold_ns;
    for (;;) {
        /* 지금 permit을 쥔 수 ≤ cap 이므로 빈 슬롯은 반드시 있다. */
        for (int i = 0; i < st->cap; i++) {
            long long zero = 0;
            if (atomic_compare_exchange_strong(&st->deadline[i], &zero, t)) {
                atomic_fetch_sub(&st->held, 1);
                return;
            }
        }
        sched_yield();
    }
}

int64_t rl_refill(ratelimit_t *rl) {
    rl_state_t *st = rl->st;
    if (!st->enabled) return 50000000;
    int64_t now = ig_now_ns(), next = INT64_MAX;
    int n = 0;
    for (int i = 0; i < st->cap; i++) {
        long long d = atomic_load(&st->deadline[i]);
        if (d == 0) continue;
        if (d <= now) {
            /* CAS로 비워야 같은 슬롯을 두 번 V() 하지 않는다 */
            if (atomic_compare_exchange_strong(&st->deadline[i], &d, 0)) n++;
        } else if (d < next) {
            next = d;
        }
    }
    for (int i = 0; i < n; i++) isem_post(&rl->permits);
    return next == INT64_MAX ? 5000000 : next - now;
}

int rl_reclaim_held(ratelimit_t *rl) {
    long h = atomic_exchange(&rl->st->held, 0);
    for (long i = 0; i < h; i++) isem_post(&rl->permits);
    return (int)h;
}
