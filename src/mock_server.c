#include "mock_server.h"

#include "common.h"

void mock_server_init(mock_server_t *ms, int limit) {
    ms->limit = limit;
    for (int i = 0; i < MS_BUCKETS; i++) {
        atomic_init(&ms->epoch[i], -1);
        atomic_init(&ms->count[i], 0);
    }
    atomic_init(&ms->admitted, 0);
    atomic_init(&ms->rejected, 0);
}

int mock_server_admit(mock_server_t *ms) {
    long long b = ig_now_ns() / (MS_BUCKET_MS * 1000000LL);
    int idx = (int)(b % MS_BUCKETS);
    long long e = atomic_load(&ms->epoch[idx]);
    if (e != b && atomic_compare_exchange_strong(&ms->epoch[idx], &e, b)) {
        atomic_store(&ms->count[idx], 0); /* 오래된 버킷 재사용 */
    }
    if (ms->limit > 0) {
        int sum = 0;
        for (int i = 0; i < MS_BUCKETS; i++) {
            long long ei = atomic_load(&ms->epoch[i]);
            if (ei > b - MS_BUCKETS && ei <= b) sum += atomic_load(&ms->count[i]);
        }
        if (sum >= ms->limit) {
            atomic_fetch_add(&ms->rejected, 1);
            return 0;
        }
    }
    atomic_fetch_add(&ms->count[idx], 1);
    atomic_fetch_add(&ms->admitted, 1);
    return 1;
}
