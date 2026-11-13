/* Readers-Writers 락 테스트.
 *  1) 상호배제: writer가 안에 있으면 reader 0명, writer 1명. reader끼리는 동시에 들어갈 수 있다.
 *  2) 일관성: writer는 (a,b)를 같은 값으로 바꾼다. reader가 a≠b를 보면 찢어진 읽기(torn read)다.
 *  3) writer 기아 없음: reader 8개가 쉬지 않고 들어오는 상황에서도 writer가 일정 시간 안에 진입해야 한다.
 */
#include <pthread.h>
#include <stdatomic.h>

#include "common.h"
#include "rwlock.h"
#include "test_util.h"

static rwlock_t L;
static atomic_int readers_in, writers_in, max_readers_in;
static volatile long A, B;
static atomic_int stop;
static atomic_long torn, excl_violation, reads, writes;
static int64_t max_writer_wait_ns;

static void spin(int n) {
    for (volatile int i = 0; i < n; i++) {
    }
}

static void *reader(void *x) {
    (void)x;
    while (!atomic_load(&stop)) {
        rw_rdlock(&L);
        int r = atomic_fetch_add(&readers_in, 1) + 1;
        int m = atomic_load(&max_readers_in);
        while (r > m && !atomic_compare_exchange_weak(&max_readers_in, &m, r)) {
        }
        if (atomic_load(&writers_in) != 0) atomic_fetch_add(&excl_violation, 1);
        long a = A;
        spin(3000);
        long b = B;
        if (a != b) atomic_fetch_add(&torn, 1);
        atomic_fetch_sub(&readers_in, 1);
        rw_rdunlock(&L);
        atomic_fetch_add(&reads, 1);
    }
    return NULL;
}

static void *writer(void *x) {
    (void)x;
    long k = 0;
    while (!atomic_load(&stop)) {
        int64_t t0 = ig_now_ns();
        rw_wrlock(&L);
        int64_t w = ig_now_ns() - t0;
        if (w > max_writer_wait_ns) max_writer_wait_ns = w; /* writer 사이는 락으로 직렬화되어 있음 */
        if (atomic_fetch_add(&writers_in, 1) != 0 || atomic_load(&readers_in) != 0)
            atomic_fetch_add(&excl_violation, 1);
        A = ++k;
        spin(100);
        B = k;
        atomic_fetch_sub(&writers_in, 1);
        rw_wrunlock(&L);
        atomic_fetch_add(&writes, 1);
        ig_sleep_ns(200000);
    }
    return NULL;
}

int main(void) {
    CHECK(rw_init(&L) == 0, "init");
    pthread_t r[8], w[2];
    for (int i = 0; i < 8; i++) pthread_create(&r[i], NULL, reader, NULL);
    for (int i = 0; i < 2; i++) pthread_create(&w[i], NULL, writer, NULL);
    ig_sleep_ns(2000000000LL);
    atomic_store(&stop, 1);
    for (int i = 0; i < 8; i++) pthread_join(r[i], NULL);
    for (int i = 0; i < 2; i++) pthread_join(w[i], NULL);

    CHECK(excl_violation == 0, "상호배제 위반 %ld", (long)excl_violation);
    PASS("상호배제: reader %ld회 / writer %ld회 동안 위반 0", (long)reads, (long)writes);
    CHECK(torn == 0, "찢어진 읽기 %ld", (long)torn);
    PASS("일관성: 찢어진 읽기(a≠b) 0");
    CHECK(max_readers_in > 1, "reader 동시 진입이 한 번도 없음");
    PASS("reader 동시 진입 최대 %d명 (공유 읽기 동작)", (int)max_readers_in);
    CHECK(writes > 100, "writer가 굶음: %ld회", (long)writes);
    CHECK(max_writer_wait_ns < 200000000LL, "writer 최대 대기 %.1fms", max_writer_wait_ns / 1e6);
    PASS("writer 기아 없음: reader 8개가 쉬지 않는 중에도 writer %ld회 진입, 최대 대기 %.2fms",
         (long)writes, max_writer_wait_ns / 1e6);
    rw_destroy(&L);
    return 0;
}
