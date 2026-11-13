/* 유한 버퍼 스트레스 테스트.
 *  1) MPMC: 생산자 P개 × K개를 소비자 C개가 꺼낸다 → 모든 원소가 정확히 한 번씩 나와야 한다(유실/중복 0).
 *     용량을 1로 줄여서 생산자와 소비자가 계속 서로를 기다리는 최악의 경우도 돌린다.
 *  2) 소비자 1개: 생산자별 FIFO 순서가 유지되어야 한다.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "bbuf.h"
#include "common.h"
#include "test_util.h"

typedef struct {
    bbuf_t *b;
    int id, k;
} prod_arg_t;

typedef struct {
    bbuf_t *b;
    atomic_uchar *seen;
    int k;
    long got, dup;
    int64_t *last_per_prod; /* 소비자 1개일 때만 사용 */
    long order_violations;
} cons_arg_t;

static void *producer(void *a) {
    prod_arg_t *p = a;
    for (int i = 0; i < p->k; i++) {
        uint64_t v = ((uint64_t)p->id << 32) | (uint32_t)i;
        bbuf_put(p->b, &v);
    }
    return NULL;
}

static void *consumer(void *a) {
    cons_arg_t *c = a;
    for (;;) {
        uint64_t v;
        bbuf_get(c->b, &v);
        if (v == UINT64_MAX) break; /* poison */
        uint32_t pid = (uint32_t)(v >> 32), i = (uint32_t)v;
        size_t idx = (size_t)pid * (size_t)c->k + i;
        if (atomic_fetch_add(&c->seen[idx], 1) != 0) c->dup++;
        if (c->last_per_prod) {
            if ((int64_t)i <= c->last_per_prod[pid]) c->order_violations++;
            c->last_per_prod[pid] = i;
        }
        c->got++;
    }
    return NULL;
}

static void run(int P, int C, int K, size_t cap) {
    bbuf_t b;
    CHECK(bbuf_init(&b, cap, sizeof(uint64_t)) == 0, "init");
    atomic_uchar *seen = calloc((size_t)P * K, 1);
    pthread_t pt[64], ct[64];
    prod_arg_t pa[64];
    cons_arg_t ca[64];
    int64_t last[64];
    for (int i = 0; i < 64; i++) last[i] = -1;
    int64_t t0 = ig_now_ns();
    for (int i = 0; i < C; i++) {
        memset(&ca[i], 0, sizeof ca[i]);
        ca[i].b = &b;
        ca[i].seen = seen;
        ca[i].k = K;
        ca[i].last_per_prod = C == 1 ? last : NULL;
        pthread_create(&ct[i], NULL, consumer, &ca[i]);
    }
    for (int i = 0; i < P; i++) {
        pa[i] = (prod_arg_t){&b, i, K};
        pthread_create(&pt[i], NULL, producer, &pa[i]);
    }
    for (int i = 0; i < P; i++) pthread_join(pt[i], NULL);
    uint64_t poison = UINT64_MAX;
    for (int i = 0; i < C; i++) bbuf_put(&b, &poison);
    long got = 0, dup = 0, ov = 0;
    for (int i = 0; i < C; i++) {
        pthread_join(ct[i], NULL);
        got += ca[i].got;
        dup += ca[i].dup;
        ov += ca[i].order_violations;
    }
    double ms = (ig_now_ns() - t0) / 1e6;
    long missing = 0;
    for (long i = 0; i < (long)P * K; i++) missing += seen[i] == 0;
    CHECK(got == (long)P * K, "got=%ld expected=%ld", got, (long)P * K);
    CHECK(dup == 0, "중복 %ld", dup);
    CHECK(missing == 0, "유실 %ld", missing);
    CHECK(ov == 0, "FIFO 위반 %ld", ov);
    /* 끝난 뒤 empty는 cap, full은 0이어야 한다(permit 보존) */
    CHECK(isem_count_slow(&b.empty) == (int)cap, "empty permit != cap");
    CHECK(isem_count_slow(&b.full) == 0, "full permit != 0");
    PASS("P=%d C=%d cap=%zu: %ld건 유실 0 중복 0%s (%.0fms, %.0f만 ops/s)", P, C, cap, got,
         C == 1 ? " FIFO 유지" : "", ms, got / ms / 10.0);
    free(seen);
    bbuf_destroy(&b);
}

int main(void) {
    run(1, 1, 200000, 64);
    run(4, 1, 50000, 16);
    run(8, 8, 25000, 64);
    run(8, 8, 10000, 1); /* 용량 1: 최대 경합 */
    run(16, 4, 5000, 3);
    return 0;
}
