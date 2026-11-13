/* 프로세스 간 테스트 (shm_open + named 세마포).
 *  1) SPSC 링: 자식 프로세스가 이름으로 링에 붙어(fork 상속이 아니라 sem_open/shm_open으로 다시 연다)
 *     N개를 순서대로 넣는다. 부모가 꺼내서 개수와 순서를 검사한다.
 *  2) 공유 레이트 리미터: 자식 프로세스 3개가 같은 permit을 쓰고 부모가 refill 한다.
 *     세 프로세스의 요청 시작 시각을 합쳐서 슬라이딩 윈도 한도를 검사한다.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bbuf.h"
#include "common.h"
#include "ratelimit.h"
#include "test_util.h"

#define N 200000
#define RCAP 256
#define LCAP 4
#define HOLD 150000000LL
#define PER_CHILD 400

typedef struct {
    rl_state_t rl;
    atomic_int nstarts;
    int64_t starts[3 * PER_CHILD];
    atomic_int stop;
    _Alignas(64) unsigned char ring[];
} shared_t;

static char shm_name[32], ring_prefix[32], rl_name[32];

static shared_t *map_shared(size_t sz, int create) {
    int fd = shm_open(shm_name, create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR, 0600);
    CHECK(fd >= 0, "shm_open");
    if (create) CHECK(ftruncate(fd, (off_t)sz) == 0, "ftruncate");
    shared_t *s = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    CHECK(s != MAP_FAILED, "mmap");
    return s;
}

static int64_t cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}
static int cmpf(const void *a, const void *b) { return (int)cmp_i64(a, b); }

int main(void) {
    snprintf(shm_name, sizeof shm_name, "/igt%d.shm", (int)getpid());
    snprintf(ring_prefix, sizeof ring_prefix, "/igt%d.r", (int)getpid());
    snprintf(rl_name, sizeof rl_name, "/igt%d.l", (int)getpid());
    size_t sz = sizeof(shared_t) + bbuf_state_size(RCAP, sizeof(uint64_t));
    shm_unlink(shm_name);
    shared_t *S = map_shared(sz, 1);
    memset(S, 0, sz);

    bbuf_t ring;
    CHECK(bbuf_create_shared(&ring, S->ring, RCAP, sizeof(uint64_t), ring_prefix) == 0, "ring create");
    ratelimit_t rl;
    CHECK(rl_create_shared(&rl, &S->rl, LCAP, HOLD, 1, rl_name) == 0, "rl create");

    /* 1) SPSC 링 */
    pid_t p = fork();
    if (p == 0) {
        shared_t *C = map_shared(sz, 0); /* 이름으로 다시 매핑 */
        bbuf_t r;
        if (bbuf_attach_shared(&r, C->ring, ring_prefix)) _exit(10);
        for (uint64_t i = 0; i < N; i++) bbuf_put(&r, &i);
        bbuf_destroy(&r);
        _exit(0);
    }
    int64_t t0 = ig_now_ns();
    long bad = 0;
    for (uint64_t i = 0; i < N; i++) {
        uint64_t v;
        bbuf_get(&ring, &v);
        bad += v != i;
    }
    int st;
    waitpid(p, &st, 0);
    double ms = (ig_now_ns() - t0) / 1e6;
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "자식 실패 %d", st);
    CHECK(bad == 0, "순서/값 오류 %ld", bad);
    PASS("프로세스 간 SPSC 링: %d건 순서 그대로 수신 (%.0fms, 프로세스 간 %.0f만 msg/s)", N, ms, N / ms / 10);

    /* 2) 프로세스 3개가 공유하는 레이트 리미터 */
    pid_t kids[3];
    for (int k = 0; k < 3; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            shared_t *C = map_shared(sz, 0);
            ratelimit_t r;
            if (rl_attach_shared(&r, &C->rl, rl_name)) _exit(11);
            for (int i = 0; i < PER_CHILD / 8; i++) {
                rl_acquire(&r);
                int idx = atomic_fetch_add(&C->nstarts, 1);
                C->starts[idx] = ig_now_ns();
                ig_sleep_ns(2000000);
                rl_release_deferred(&r);
            }
            rl_destroy(&r);
            _exit(0);
        }
    }
    int alive = 3;
    while (alive > 0) {
        int64_t w = rl_refill(&rl);
        ig_sleep_ns(w < 200000 ? 200000 : (w > 5000000 ? 5000000 : w));
        for (int k = 0; k < 3; k++)
            if (kids[k] > 0 && waitpid(kids[k], &st, WNOHANG) == kids[k]) {
                CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "rl 자식 실패");
                kids[k] = -1;
                alive--;
            }
    }
    int n = atomic_load(&S->nstarts);
    qsort(S->starts, (size_t)n, sizeof(int64_t), cmpf);
    int worst = 0, j = 0;
    for (int i = 0; i < n; i++) {
        while (S->starts[i] - S->starts[j] >= HOLD) j++;
        if (i - j + 1 > worst) worst = i - j + 1;
    }
    CHECK(worst <= LCAP, "프로세스 합산 %d건 > 한도 %d", worst, LCAP);
    PASS("프로세스 3개 공유 permit: 요청 %d건, 어떤 %lldms 구간에서도 합계 최대 %d건 (한도 %d)", n, HOLD / 1000000,
         worst, LCAP);

    bbuf_destroy(&ring);
    rl_destroy(&rl);
    munmap(S, sz);
    shm_unlink(shm_name);
    return 0;
}
