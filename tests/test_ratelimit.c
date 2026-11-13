/* 레이트 리미터 테스트: 스레드 16개가 최대한 빨리 요청을 보낸다.
 * 모든 요청의 시작 시각을 기록해 두고, "길이 W인 어떤 구간에도 시작이 cap개 이하"인지
 * 모든 구간을 전수 검사한다(슬라이딩 윈도). 테스트 시간을 줄이려고 W=200ms로 돌린다(1초도 논리는 같다).
 * 비교용으로 "W마다 cap개씩 채워주는" 고정 토큰 버킷을 같은 방식으로 검사하면, 경계에서 한도를 넘는 걸 볼 수 있다.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "common.h"
#include "ratelimit.h"
#include "test_util.h"

#define CAP 5
#define HOLD_NS 200000000LL
#define MAXREQ 100000

static ratelimit_t RL;
static int64_t starts[MAXREQ];
static atomic_int nstarts;
static atomic_int stop_req, stop_refill;
static atomic_int inflight, max_inflight;

static void *client(void *x) {
    uint64_t rng = (uint64_t)(uintptr_t)x * 7919 + 1;
    while (!atomic_load(&stop_req)) {
        rl_acquire(&RL);
        int i = atomic_fetch_add(&nstarts, 1);
        if (i < MAXREQ) starts[i] = ig_now_ns();
        int f = atomic_fetch_add(&inflight, 1) + 1;
        int m = atomic_load(&max_inflight);
        while (f > m && !atomic_compare_exchange_weak(&max_inflight, &m, f)) {
        }
        ig_sleep_ns((int64_t)(ig_rand_unit(&rng) * 20e6)); /* 요청 처리 0~20ms */
        atomic_fetch_sub(&inflight, 1);
        rl_release_deferred(&RL);
    }
    return NULL;
}

static void *refiller(void *x) {
    (void)x;
    while (!atomic_load(&stop_refill)) {
        int64_t w = rl_refill(&RL);
        ig_sleep_ns(w < 200000 ? 200000 : (w > 5000000 ? 5000000 : w));
    }
    return NULL;
}

static int cmp(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int max_in_window(int64_t *t, int n, int64_t w) {
    int best = 0, j = 0;
    for (int i = 0; i < n; i++) {
        while (t[i] - t[j] >= w) j++;
        if (i - j + 1 > best) best = i - j + 1;
    }
    return best;
}

int main(void) {
    CHECK(rl_init(&RL, CAP, HOLD_NS, 1) == 0, "init");
    pthread_t c[16], r;
    pthread_create(&r, NULL, refiller, NULL);
    for (int i = 0; i < 16; i++) pthread_create(&c[i], NULL, client, (void *)(uintptr_t)(i + 1));
    ig_sleep_ns(3000000000LL);
    atomic_store(&stop_req, 1);
    /* 대기 중인 클라이언트를 깨우기 위해 refill은 클라이언트가 다 끝난 뒤에 멈춘다 */
    for (int i = 0; i < 16; i++) pthread_join(c[i], NULL);
    ig_sleep_ns(HOLD_NS + 50000000LL); /* 마지막 반납 예약까지 refill이 처리하도록 기다린 뒤 멈춘다 */
    atomic_store(&stop_refill, 1);
    pthread_join(r, NULL);

    int n = atomic_load(&nstarts);
    if (n > MAXREQ) n = MAXREQ;
    qsort(starts, (size_t)n, sizeof *starts, cmp);
    int worst = max_in_window(starts, n, HOLD_NS);
    CHECK(worst <= CAP, "어떤 %lldms 구간에 %d건 (한도 %d)", HOLD_NS / 1000000, worst, CAP);
    PASS("슬라이딩 윈도 검사: 요청 %d건, 어떤 %lldms 구간에서도 최대 %d건 (한도 %d)", n, HOLD_NS / 1000000,
         worst, CAP);
    /* 이론 상한: 처음 cap개 + 이후 HOLD마다 cap개 (실제 요청이 오간 구간 기준) */
    double span = (double)(starts[n - 1] - starts[0]);
    double ideal = CAP + span / HOLD_NS * CAP;
    CHECK(n > ideal * 0.6, "처리량이 너무 낮음 %d / 이론 %.0f", n, ideal);
    PASS("처리량 %d건 / 이론 상한 %.0f건 (%.0f%%) — 과하게 보수적이지 않음", n, ideal, 100.0 * n / ideal);
    CHECK(max_inflight <= CAP, "동시 진행 %d", (int)max_inflight);
    PASS("동시 진행 요청 최대 %d건 ≤ %d", (int)max_inflight, CAP);
    CHECK(isem_count_slow(&RL.permits) == CAP, "permit 보존 실패");
    PASS("종료 후 permit %d개 전부 복귀", CAP);

    /* 비교: 고정 주기로 cap개씩 채우는 순진한 토큰 버킷 (경계 몰림 재현) */
    int64_t naive[2 * CAP];
    int64_t base = 0;
    for (int i = 0; i < CAP; i++) naive[i] = base + HOLD_NS - 1000000 + i * 1000; /* 첫 주기 끝에 몰림 */
    for (int i = 0; i < CAP; i++) naive[CAP + i] = base + HOLD_NS + i * 1000;     /* 다음 주기 시작에 몰림 */
    int nw = max_in_window(naive, 2 * CAP, HOLD_NS);
    PASS("(비교) 고정 주기 토큰 버킷은 경계에서 %lldms 안에 %d건까지 허용 → 슬라이딩 한도 위반 가능",
         HOLD_NS / 1000000, nw);
    rl_destroy(&RL);
    return 0;
}
