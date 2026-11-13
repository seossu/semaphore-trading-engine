/* 시세 수집 프로세스 (엔진이 posix_spawn으로 띄운다).
 *
 * 왜 스레드가 아니라 프로세스인가:
 *   - 장애 격리: 시세 조회(libcurl, 외부 네트워크)에서 크래시가 나도 주문/포지션을 가진 엔진은 살아 있다.
 *     엔진의 supervisor가 SIGCHLD로 감지해서 새로 띄우면, 새 feeder는 같은 이름의 세마포와
 *     공유메모리에 다시 붙어서 이어서 일한다. (kill -9 <feeder pid>로 직접 확인 가능)
 *   - 이름으로 붙는다는 것: fork 상속이 아니라 sem_open(name)/shm_open(name)으로 연결하므로,
 *     멀티스레드 상태인 엔진에서 fork 없이(posix_spawn = fork+exec) 안전하게 재시작할 수 있다.
 *
 * 공유 자원:
 *   md 링(SPSC)  : 이 프로세스가 유일한 생산자, 엔진의 md_receiver가 유일한 소비자
 *   rl_md        : 시세 조회 permit. 여기서 P, 엔진 refill 스레드가 V
 *   tok_ready    : KIS 토큰이 처음 준비되면 엔진이 V (일회성 신호 세마포)
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>

#include "engine.h"

static atomic_int g_stop;

static void on_term(int sig) {
    (void)sig;
    atomic_store(&g_stop, 1); /* 원자 store는 lock-free라 시그널 핸들러에서 안전 */
}

static double bits_to_d(long long b) {
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}
static long long d_to_bits(double d) {
    long long b;
    memcpy(&b, &d, sizeof b);
    return b;
}

typedef struct {
    double px, drift;
    int regime_left;
    uint64_t rng;
} mock_px_t;

/* 국면 전환이 있는 랜덤워크. 추세 구간이 있어야 돌파 전략이 실제로 매매를 한다. */
static int mock_price(mock_px_t *m, mock_server_t *srv, double *out) {
    if (!mock_server_admit(srv)) return KIS_RATE_LIMITED;
    ig_sleep_ns((int64_t)((3 + ig_rand_unit(&m->rng) * 7) * 1e6)); /* 네트워크 지연 흉내 */
    if (m->regime_left-- <= 0) {
        double u = ig_rand_unit(&m->rng);
        m->drift = u < 0.35 ? 0.0003 : u < 0.70 ? -0.0003 : 0.0;
        m->regime_left = 10 + (int)(ig_rand_unit(&m->rng) * 30);
    }
    m->px *= exp(m->drift + 0.0007 * ig_rand_normal(&m->rng));
    *out = round(m->px / 0.05) * 0.05; /* KOSPI200 선물 호가단위 0.05 */
    return KIS_OK;
}

int feeder_main(const char *shm_name) {
    ig_set_role("feeder");
    signal(SIGINT, SIG_IGN); /* 터미널 Ctrl+C는 엔진만 받는다. feeder는 엔진의 SIGTERM으로만 종료 */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART 없음 → sem_wait가 EINTR로 깨어나서 stop을 확인할 수 있다 */
    sigaction(SIGTERM, &sa, NULL);

    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) {
        ig_log("feeder", "shm_open(%s) 실패: %s", shm_name, strerror(errno));
        return 1;
    }
    struct stat stt;
    fstat(fd, &stt);
    shm_t *sh = mmap(NULL, (size_t)stt.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (sh == MAP_FAILED || sh->magic != SHM_MAGIC) {
        ig_log("feeder", "공유메모리 매핑 실패");
        return 1;
    }

    char n[32];
    bbuf_t ring;
    ratelimit_t rl;
    isem_t tok_ready;
    snprintf(n, sizeof n, "%s.md", sh->ipc_prefix);
    if (bbuf_attach_shared(&ring, sh->ring_mem, n)) return 1;
    snprintf(n, sizeof n, "%s.rm", sh->ipc_prefix);
    if (rl_attach_shared(&rl, &sh->rl_md, n)) return 1;
    snprintf(n, sizeof n, "%s.tk", sh->ipc_prefix);
    if (isem_open_named(&tok_ready, n)) return 1;

    long starts = atomic_fetch_add(&sh->feeder_starts, 1) + 1;
    ig_log("feeder", "시작 pid=%d (%ld번째 기동), 공유메모리/세마포 이름으로 연결 완료", (int)getpid(), starts);

    pid_t parent = getppid();
    kis_t kis;
    if (sh->price_src == SRC_KIS) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        kis_init_from_env(&kis, &sh->tok, sh->inst);
        if (atomic_load(&sh->tok.ver) == 0) {
            ig_log("feeder", "KIS 토큰 대기 (tok_ready 세마포)");
            if (isem_wait_intr(&tok_ready, &g_stop)) goto out;
        }
    }

    mock_px_t m;
    memset(&m, 0, sizeof m);
    m.rng = sh->seed ^ ((uint64_t)getpid() << 32) ^ (uint64_t)starts;
    double last = bits_to_d(atomic_load(&sh->last_px_bits));
    m.px = last > 0 ? last : 1040.0;

    uint64_t seq = (uint64_t)starts << 40; /* 재기동해도 seq가 겹치지 않게 */
    int64_t last_err_log = 0;
    while (!atomic_load(&g_stop) && !atomic_load(&sh->stop)) {
        if (getppid() != parent) break; /* 엔진이 죽으면 고아로 남지 않고 스스로 종료 */
        int64_t t0 = ig_now_ns();
        if (rl_acquire_intr(&rl, &g_stop)) break;
        double px = 0;
        char err[160] = "";
        int rc = sh->price_src == SRC_KIS ? kis_get_price(&kis, &px, err, sizeof err)
                                          : mock_price(&m, &sh->srv, &px);
        rl_release_deferred(&rl);
        atomic_fetch_add(&sh->polls, 1);
        if (rc == KIS_OK) {
            quote_t q;
            memset(&q, 0, sizeof q);
            snprintf(q.sym, sizeof q.sym, "%s", sh->inst);
            q.price = px;
            q.seq = ++seq;
            q.t_feed_ns = ig_now_ns();
            q.wall_ms = ig_wall_ms();
            atomic_store(&sh->last_px_bits, d_to_bits(px));
            if (seq == ((uint64_t)starts << 40) + 1) ig_log("feeder", "첫 시세 %s = %.2f", sh->inst, px);
            if (!bbuf_tryput(&ring, &q)) atomic_fetch_add(&sh->ring_dropped, 1);
        } else {
            if (rc == KIS_RATE_LIMITED) atomic_fetch_add(&sh->poll_ratelimited, 1);
            else atomic_fetch_add(&sh->poll_err, 1);
            if (rc == KIS_TOKEN_EXPIRED) atomic_store(&sh->tok.refresh_req, 1);
            if (rc != KIS_RATE_LIMITED && ig_now_ns() - last_err_log > 5000000000LL) {
                last_err_log = ig_now_ns();
                ig_log("feeder", "시세 조회 실패: %s", err);
            }
        }
        int64_t left = sh->feed_min_interval_ns - (ig_now_ns() - t0);
        if (left > 0) ig_sleep_until_stop(left, &g_stop);
    }
out:
    ig_log("feeder", "종료 pid=%d", (int)getpid());
    bbuf_destroy(&ring);
    rl_destroy(&rl);
    isem_close(&tok_ready);
    munmap(sh, (size_t)stt.st_size);
    return 0;
}
