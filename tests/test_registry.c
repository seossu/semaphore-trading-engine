/* registry의 "등록 vs 체결 도착" 경쟁 테스트.
 * 워커 스레드 4개는 bind(등록)를, 거래소 스레드 2개는 route(체결)를 같은 주문번호 집합에 대해
 * 무작위 지연을 두고 동시에 수행한다. 어느 쪽이 먼저 오든:
 *   - 모든 체결이 정확히 한 번 전달되어야 하고(유실/중복 0)
 *   - 같은 주문의 RECEIVED가 FILLED보다 먼저여야 한다.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "registry.h"
#include "test_util.h"

#define N 20000
static registry_t R;
static atomic_long seq;
static atomic_long recv_seq[N], fill_seq[N];
static atomic_int deliveries[N];
static atomic_long via_park;

static void odno_of(int i, char *o) { snprintf(o, IG_ODNO_LEN, "T%08d", i); }

static void mark_received(void *arg) {
    int i = (int)(intptr_t)arg;
    atomic_store(&recv_seq[i], atomic_fetch_add(&seq, 1) + 1);
}

static void deliver(int i) {
    atomic_store(&fill_seq[i], atomic_fetch_add(&seq, 1) + 1);
    atomic_fetch_add(&deliveries[i], 1);
}

static void jitter(uint64_t *rng) {
    double u = ig_rand_unit(rng);
    if (u < 0.3) return;
    if (u < 0.9) {
        for (volatile int k = 0; k < (int)(u * 2000); k++) {
        }
        return;
    }
    ig_sleep_ns(20000);
}

#define ROUND 200 /* 엔진에서 미체결 주문 수가 inflight 세마포로 묶이는 것과 같은 조건 */
static int round_base;

static void *worker(void *x) {
    int w = (int)(intptr_t)x;
    uint64_t rng = (uint64_t)w * 1315423911u + 7 + (uint64_t)round_base;
    for (int i = round_base + w; i < round_base + ROUND; i += 4) {
        jitter(&rng);
        /* 등록을 가끔 늦춰 "등록 전 체결 도착"(park 경로)이 확실히 생기게 한다.
         * TSan 빌드는 스레드 실행 순서가 달라져 이 경쟁이 드물어지기 때문이다. */
        if (ig_rand_unit(&rng) < 0.05) ig_sleep_ns(50000);
        reg_entry_t e;
        memset(&e, 0, sizeof e);
        odno_of(i, e.odno);
        e.cid = (uint64_t)i;
        ack_t out[4];
        int n = reg_bind(&R, &e, mark_received, (void *)(intptr_t)i, out, 4);
        for (int k = 0; k < n; k++) {
            CHECK(out[k].cid == (uint64_t)i, "park에서 받은 체결의 cid 불일치");
            atomic_fetch_add(&via_park, 1);
            deliver(i);
        }
    }
    return NULL;
}

static void *exchange(void *x) {
    int w = (int)(intptr_t)x;
    uint64_t rng = (uint64_t)w * 2654435761u + 3 + (uint64_t)round_base;
    for (int i = round_base + w; i < round_base + ROUND; i += 2) {
        jitter(&rng);
        ack_t a;
        memset(&a, 0, sizeof a);
        a.type = ACK_FILLED;
        odno_of(i, a.odno);
        if (reg_route(&R, &a)) {
            CHECK(a.cid == (uint64_t)i, "route cid 불일치");
            deliver(i);
        }
    }
    return NULL;
}

int main(void) {
    CHECK(reg_init(&R) == 0, "init");
    pthread_t w[4], x[2];
    for (round_base = 0; round_base < N; round_base += ROUND) {
        for (int i = 0; i < 4; i++) pthread_create(&w[i], NULL, worker, (void *)(intptr_t)i);
        for (int i = 0; i < 2; i++) pthread_create(&x[i], NULL, exchange, (void *)(intptr_t)i);
        for (int i = 0; i < 4; i++) pthread_join(w[i], NULL);
        for (int i = 0; i < 2; i++) pthread_join(x[i], NULL);
    }
    CHECK(R.park_overflow == 0, "park overflow");

    long lost = 0, dup = 0, order = 0;
    for (int i = 0; i < N; i++) {
        int d = atomic_load(&deliveries[i]);
        lost += d == 0;
        dup += d > 1;
        order += !(atomic_load(&recv_seq[i]) > 0 && atomic_load(&recv_seq[i]) < atomic_load(&fill_seq[i]));
    }
    CHECK(lost == 0, "유실 %ld", lost);
    CHECK(dup == 0, "중복 %ld", dup);
    CHECK(order == 0, "RECEIVED보다 FILLED가 먼저인 주문 %ld", order);
    CHECK(reg_parked_now(&R) == 0, "남은 park");
    CHECK(atomic_load(&via_park) > 0, "park 경로가 한 번도 실행되지 않음(테스트가 경쟁을 못 만듦)");
    PASS("주문 %d건: 유실 0, 중복 0, RECEIVED→FILLED 순서 위반 0 (등록 전 체결 도착 %ld건 = park 경로)", N,
         (long)via_park);
    reg_destroy(&R);
    return 0;
}
