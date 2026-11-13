/* 진입점: 설정 → 공유메모리/named 세마포 생성 → feeder 프로세스 기동 → 엔진 스레드 기동
 *        → 메인 스레드는 sigwait로 시그널만 처리(supervisor) → 순서대로 종료 → 결과/불변식 검증.
 *
 * 시그널 처리: 스레드를 만들기 전에 메인에서 SIGINT/SIGTERM/SIGALRM/SIGCHLD를 막아 둔다.
 *   새 스레드는 이 마스크를 물려받으므로, 시그널은 sigwait 중인 메인 스레드에서만 동기적으로 받는다.
 *   임의의 스레드가 세마포를 쥔 채 비동기 핸들러에 끌려가는 일이 없다.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "engine.h"

extern char **environ;

static void usage(void) {
    fprintf(stderr,
            "usage: ignacio [options]\n"
            "  --mode mock|kis-dry|kis-live  mock: 시세/주문 모두 모의 (기본)\n"
            "                                kis-dry: KIS 실시세 + 모의 체결 (주문 안 나감)\n"
            "                                kis-live: KIS 모의투자 계좌로 실제 주문\n"
            "  --duration SEC     실행 시간 (mock 기본 60, kis 기본 0=Ctrl+C까지)\n"
            "  --strategies N     전략 스레드 수 (mock 기본 8)\n"
            "  --workers M        게이트웨이 워커 수 (기본 4)\n"
            "  --max-net N        계좌 롱 한도 계약수 = exposure 세마포 초기값 (기본 3)\n"
            "  --no-ratelimit     레이트 리미터 끄기 (비교 실험용)\n"
            "  --server-limit N   모의 서버 초당 허용 건수 (기본 20)\n"
            "  --md-permits N --trade-permits N  초당 permit 배분 (mock 기본 8/10, kis 기본 1/1)\n"
            "  --fast-fill P      즉시 체결 비율 (registry 경쟁 경로 검증용, 기본 0.15)\n"
            "  --inst CODE        종목코드 (kis 기본: env KIS_INST_CODE)\n"
            "  --env PATH         .env 경로\n"
            "  --out DIR          결과 폴더 (기본 results)\n"
            "  --seed N  --verbose\n");
}

static void defaults(config_t *c, int kis) {
    memset(c, 0, sizeof *c);
    c->nstrat = kis ? 4 : 8;
    c->nworkers = 4;
    c->rl_enabled = 1;
    c->rl_md_cap = kis ? 1 : 8;
    c->rl_trade_cap = kis ? 1 : 10;
    c->rl_hold_ns = 1050000000LL; /* 1초 + 50ms 여유 */
    c->server_limit = 20; /* 실전 KIS 초당 20건 */
    c->feed_min_interval_ns = kis ? 1000000000LL : 50000000LL;
    c->nav_interval_ns = kis ? 60000000000LL : 2000000000LL;
    c->bar_ms = kis ? 60000 : 1000;
    c->loss_pct = kis ? 0.5 : 0.15;
    c->tp_pct = kis ? 1.0 : 0.25;
    c->qty = 1;
    c->max_net = 3;
    c->inflight_cap = 2;
    c->exit_hhmmss = 153000;
    c->duration_s = kis ? 0 : 60;
    c->flatten_before_end_s = kis ? 0 : 6;
    c->fast_fill_prob = kis ? 0 : 0.15;
    c->reject_prob = kis ? 0 : 0.03;
    c->seed = 20261101;
    snprintf(c->out_dir, sizeof c->out_dir, "results");
}

static int parse_args(int argc, char **argv, config_t *c) {
    const char *mode = "mock";
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) mode = argv[i + 1];
    int kis = strcmp(mode, "mock") != 0;
    defaults(c, kis);
    if (strcmp(mode, "mock") == 0) {
        c->price_src = SRC_MOCK;
        c->exec_src = SRC_MOCK;
    } else if (strcmp(mode, "kis-dry") == 0) {
        c->price_src = SRC_KIS;
        c->exec_src = SRC_MOCK;
    } else if (strcmp(mode, "kis-live") == 0) {
        c->price_src = SRC_KIS;
        c->exec_src = SRC_KIS;
    } else {
        usage();
        return -1;
    }
    c->mode_name = mode;

    static struct option opts[] = {
        {"mode", required_argument, 0, 'm'},          {"duration", required_argument, 0, 'd'},
        {"strategies", required_argument, 0, 's'},    {"workers", required_argument, 0, 'w'},
        {"max-net", required_argument, 0, 'n'},       {"no-ratelimit", no_argument, 0, 'R'},
        {"server-limit", required_argument, 0, 'L'},  {"md-permits", required_argument, 0, 'a'},
        {"trade-permits", required_argument, 0, 'b'}, {"fast-fill", required_argument, 0, 'f'},
        {"inst", required_argument, 0, 'i'},          {"env", required_argument, 0, 'e'},
        {"out", required_argument, 0, 'o'},           {"seed", required_argument, 0, 'S'},
        {"verbose", no_argument, 0, 'v'},             {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};
    int ch;
    while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (ch) {
        case 'm': break;
        case 'd': c->duration_s = atoi(optarg); break;
        case 's': c->nstrat = atoi(optarg); break;
        case 'w': c->nworkers = atoi(optarg); break;
        case 'n': c->max_net = atoi(optarg); break;
        case 'R': c->rl_enabled = 0; break;
        case 'L': c->server_limit = atoi(optarg); break;
        case 'a': c->rl_md_cap = atoi(optarg); break;
        case 'b': c->rl_trade_cap = atoi(optarg); break;
        case 'f': c->fast_fill_prob = atof(optarg); break;
        case 'i': snprintf(c->inst, sizeof c->inst, "%s", optarg); break;
        case 'e': snprintf(c->env_path, sizeof c->env_path, "%s", optarg); break;
        case 'o': snprintf(c->out_dir, sizeof c->out_dir, "%s", optarg); break;
        case 'S': c->seed = strtoull(optarg, NULL, 10); break;
        case 'v': c->verbose = 1; break;
        default: usage(); return -1;
        }
    }
    if (c->nstrat < 1 || c->nstrat > IG_MAX_STRAT || c->nworkers < 1 || c->nworkers > IG_MAX_WORKERS ||
        c->max_net < 1 || c->rl_md_cap < 1 || c->rl_trade_cap < 1 ||
        c->rl_md_cap > RL_MAX_PERMITS || c->rl_trade_cap > RL_MAX_PERMITS) {
        fprintf(stderr, "잘못된 설정 값\n");
        return -1;
    }
    /* inbox.h 용량 논증: ACK_CAP ≥ 2 × inflight_cap */
    if (INBOX_ACK_CAP < 2 * c->inflight_cap) {
        fprintf(stderr, "ACK_CAP < 2*inflight_cap: 교착 가능 설정\n");
        return -1;
    }
    return 0;
}

static void on_sigchld_noop(int sig) { (void)sig; }

static pid_t spawn_feeder(engine_t *e) {
    char a2[64];
    snprintf(a2, sizeof a2, "--shm=%s", e->shm_name);
    char *argv[] = {e->exe_path, "--role=feeder", a2, NULL};
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t empty, def;
    sigemptyset(&empty);
    sigemptyset(&def);
    sigaddset(&def, SIGTERM);
    sigaddset(&def, SIGCHLD);
    sigaddset(&def, SIGALRM);
    posix_spawnattr_setsigmask(&at, &empty); /* 엔진이 막아 둔 시그널 마스크를 자식에게 물려주지 않음 */
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, e->exe_path, NULL, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    if (rc != 0) {
        ig_log("supervisor", "posix_spawn 실패: %s", strerror(rc));
        return -1;
    }
    return pid;
}

static int setup_ipc(engine_t *e) {
    snprintf(e->shm->ipc_prefix, sizeof e->shm->ipc_prefix, "/ig%d", (int)getpid());
    char n[32];
    snprintf(n, sizeof n, "%s.md", e->shm->ipc_prefix);
    if (bbuf_create_shared(&e->md_ring, e->shm->ring_mem, MD_RING_CAP, sizeof(quote_t), n)) return -1;
    snprintf(n, sizeof n, "%s.rm", e->shm->ipc_prefix);
    if (rl_create_shared(&e->rl_md, &e->shm->rl_md, e->cfg.rl_md_cap, e->cfg.rl_hold_ns, e->cfg.rl_enabled, n))
        return -1;
    snprintf(n, sizeof n, "%s.tk", e->shm->ipc_prefix);
    if (isem_create_named(&e->tok_ready, n, 0)) return -1;
    return 0;
}

static int create_shm(engine_t *e) {
    snprintf(e->shm_name, sizeof e->shm_name, "/ig%d.shm", (int)getpid());
    e->shm_size = sizeof(shm_t) + bbuf_state_size(MD_RING_CAP, sizeof(quote_t));
    shm_unlink(e->shm_name);
    int fd = shm_open(e->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)e->shm_size) != 0) {
        close(fd);
        return -1;
    }
    e->shm = mmap(NULL, e->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (e->shm == MAP_FAILED) return -1;
    memset(e->shm, 0, e->shm_size);
    shm_t *s = e->shm;
    s->price_src = e->cfg.price_src;
    snprintf(s->inst, sizeof s->inst, "%s", e->cfg.inst);
    s->feed_min_interval_ns = e->cfg.rl_enabled ? 0 : e->cfg.feed_min_interval_ns;
    if (e->cfg.price_src == SRC_KIS) s->feed_min_interval_ns = e->cfg.feed_min_interval_ns;
    s->seed = e->cfg.seed;
    mock_server_init(&s->srv, e->cfg.price_src == SRC_MOCK || e->cfg.exec_src == SRC_MOCK ? e->cfg.server_limit : 0);
    s->magic = SHM_MAGIC;
    return 0;
}

/* ---------- 결과 요약 + 동기화 불변식 검증 ---------- */
static FILE *g_sum;
static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_sum) {
        va_start(ap, fmt);
        vfprintf(g_sum, fmt, ap);
        va_end(ap);
    }
}

static void lat_line(const char *label, samples_t *s) {
    if (s->n == 0) {
        out("  %-28s (샘플 없음)\n", label);
        return;
    }
    out("  %-28s n=%-7zu p50=%8.1fus  p99=%8.1fus  max=%8.1fus\n", label, s->n, samples_pct(s, 50) / 1e3,
        samples_pct(s, 99) / 1e3, samples_pct(s, 100) / 1e3);
}

static int report(engine_t *e, const char *dir, double run_s) {
    char path[700];
    snprintf(path, sizeof path, "%s/summary.txt", dir);
    g_sum = fopen(path, "w");
    const config_t *c = &e->cfg;
    shm_t *sh = e->shm;
    book_view_t v;
    book_snapshot(&e->book, &v);
    int fails = 0;

    out("\n================ Ignacio-C 실행 결과 ================\n");
    out("모드 %s | %.1f초 | 전략 스레드 %d | 게이트웨이 워커 %d | 레이트리미터 %s (시세 %d/s, 주문 %d/s) | "
        "모의서버 한도 %d/s\n",
        c->mode_name, run_s, c->nstrat, c->nworkers, c->rl_enabled ? "ON" : "OFF", c->rl_md_cap,
        c->rl_trade_cap, sh->srv.limit);

    long md_drop = 0;
    for (int i = 0; i < c->nstrat; i++) md_drop += atomic_load(&e->inbox[i].md_dropped);
    out("\n[시세] feeder 조회 %ld (실패 %ld, 속도제한 거절 %ld) | 링 드롭 %ld | 엔진 수신 %ld | "
        "전략 우편함 드롭 %ld | feeder 기동 %ld회\n",
        atomic_load(&sh->polls), atomic_load(&sh->poll_err), atomic_load(&sh->poll_ratelimited),
        atomic_load(&sh->ring_dropped), atomic_load(&e->quotes_in), md_drop, atomic_load(&sh->feeder_starts));
    long rl_total = atomic_load(&e->rl_rejected_trade) + atomic_load(&sh->poll_ratelimited);
    out("[주문] 전송 %ld | 접수 %ld | 체결 %ld | 거절 %ld (그중 EGW00201 %ld) | 잔고조회 성공 %ld 실패 %ld\n",
        atomic_load(&e->orders_sent), atomic_load(&e->acks_received), atomic_load(&e->acks_filled),
        atomic_load(&e->acks_rejected), atomic_load(&e->rl_rejected_trade), atomic_load(&e->nav_ok),
        atomic_load(&e->nav_fail));
    out("[속도제한] 서버가 거절한 요청 합계(EGW00201) = %ld  ← 레이트리미터 ON이면 0이어야 함\n", rl_total);

    out("\n[전략별]\n");
    long risk_blocked = 0;
    for (int i = 0; i < c->nstrat; i++) {
        strategy_report(e->strat[i], stdout);
        if (g_sum) strategy_report(e->strat[i], g_sum);
        out(" 실현 %+.2fpt\n", v.pos[i].realized_pts);
        risk_blocked += strategy_counter(e->strat[i], SC_RISK_BLOCKED);
    }
    out("  합계 실현손익 %+.2fpt (%+.0f원, 수수료 제외)\n", v.realized_pts, v.realized_pts * IG_KRW_PER_POINT);

    samples_t mdq, recv, fill, gw_wait, gw_place;
    samples_init(&mdq, 2000000);
    samples_init(&recv, 200000);
    samples_init(&fill, 200000);
    samples_init(&gw_wait, 200000);
    samples_init(&gw_place, 200000);
    for (int i = 0; i < c->nstrat; i++) strategy_collect_latency(e->strat[i], &mdq, &recv, &fill);
    for (int i = 0; i < c->nworkers; i++) {
        samples_merge(&gw_wait, &e->gw_wait[i]);
        samples_merge(&gw_place, &e->gw_place[i]);
    }
    out("\n[지연시간]\n");
    lat_line("시세: feeder→전략 (IPC 포함)", &mdq);
    lat_line("주문: 전략→워커 큐 대기", &gw_wait);
    lat_line("주문: 브로커 호출(place)", &gw_place);
    lat_line("시세→접수 ACK (end-to-end)", &recv);
    lat_line("시세→체결 ACK (end-to-end)", &fill);
    samples_free(&mdq);
    samples_free(&recv);
    samples_free(&fill);
    samples_free(&gw_wait);
    samples_free(&gw_place);

    /* 모든 스레드가 join된 뒤라 세마포 값을 trywait로 세어도 경쟁이 없다 */
    out("\n[동기화 불변식 검증]\n");
    int exp_free = isem_count_slow(&e->exposure), held = 0;
    for (int i = 0; i < c->nstrat; i++) held += strategy_holding(e->strat[i]);
    int ok = exp_free + held == c->max_net;
    fails += !ok;
    out("  %s exposure permit 보존: 남은 %d + 보유 %d = 초기값 %d\n", ok ? "OK  " : "FAIL", exp_free, held,
        c->max_net);

    int infl_ok = 1;
    for (int i = 0; i < c->nstrat; i++) infl_ok &= strategy_inflight_free(e->strat[i]) == c->inflight_cap;
    fails += !infl_ok;
    out("  %s 전략별 inflight permit 전부 반납 (각 %d)\n", infl_ok ? "OK  " : "FAIL", c->inflight_cap);

    long ovf = 0;
    for (int i = 0; i < c->nstrat; i++) ovf += atomic_load(&e->inbox[i].ack_overflow);
    fails += ovf != 0;
    out("  %s ACK 큐 overflow = %ld (용량 논증: ACK_CAP %d ≥ 2×inflight %d)\n", ovf == 0 ? "OK  " : "FAIL",
        ovf, INBOX_ACK_CAP, c->inflight_cap);

    long fills_by_book = v.fills;
    ok = fills_by_book == atomic_load(&e->acks_filled);
    fails += !ok;
    out("  %s 체결 유실 없음: 장부 반영 %ld = 전략 전달 %ld\n", ok ? "OK  " : "FAIL", fills_by_book,
        atomic_load(&e->acks_filled));

    int parked = reg_parked_now(&e->reg);
    fails += parked != 0 || e->reg.park_overflow != 0;
    out("  %s registry 경쟁 경로: 등록 전 도착한 체결 %ld건 park → 미해결 %d건\n", parked == 0 ? "OK  " : "FAIL",
        e->reg.parked_total, parked);

    if (c->exec_src == SRC_MOCK) {
        ok = e->ledger_net == v.net;
        fails += !ok;
        out("  %s 거래소 원장 순포지션 %d = 내부 장부 %d\n", ok ? "OK  " : "FAIL", e->ledger_net, v.net);
    }
    if (c->duration_s > 0) {
        ok = v.net == 0;
        fails += !ok;
        out("  %s 장마감 전량청산 후 순포지션 = %d\n", ok ? "OK  " : "FAIL", v.net);
    }
    if (c->rl_enabled) {
        fails += rl_total != 0;
        out("  %s 레이트리미터 ON → 서버 EGW00201 = %ld\n", rl_total == 0 ? "OK  " : "FAIL", rl_total);
    }
    out("  (참고) 계좌 한도 때문에 진입을 보류한 횟수 %ld, NAV 지속 불일치 %ld\n", risk_blocked,
        atomic_load(&e->nav_mismatch_persistent));
    out("\n결과 폴더: %s  (summary.txt, trades.csv)\n", dir);
    out("불변식 %s\n", fails == 0 ? "전부 통과" : "실패 있음");
    if (g_sum) fclose(g_sum);
    g_sum = NULL;
    return fails;
}

static void find_env(config_t *c) {
    if (c->env_path[0]) {
        if (ig_load_env_file(c->env_path) < 0) ig_log("main", ".env 열기 실패: %s", c->env_path);
        return;
    }
    const char *cands[] = {".env"};
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++)
        if (ig_load_env_file(cands[i]) >= 0) {
            snprintf(c->env_path, sizeof c->env_path, "%s", cands[i]);
            return;
        }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--role=feeder") == 0) {
            for (int j = 1; j < argc; j++)
                if (strncmp(argv[j], "--shm=", 6) == 0) return feeder_main(argv[j] + 6);
            return 2;
        }

    static engine_t E;
    engine_t *e = &E;
    if (parse_args(argc, argv, &e->cfg)) return 2;
    config_t *c = &e->cfg;
    if (!realpath(argv[0], e->exe_path)) {
        perror("realpath");
        return 2;
    }

    /* named 세마포는 macOS에서 파일 디스크립터를 하나씩 쓴다 → 한도를 넉넉히 */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
        rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    if (c->price_src == SRC_KIS || c->exec_src == SRC_KIS) {
        find_env(c);
        if (!c->inst[0]) snprintf(c->inst, sizeof c->inst, "%s", ig_getenv("KIS_INST_CODE", ""));
        if (!c->inst[0]) {
            fprintf(stderr, "KIS 모드는 종목코드가 필요합니다: --inst <단축코드> 또는 KIS_INST_CODE\n");
            return 2;
        }
        ig_log("main", ".env = %s, 종목 = %s", c->env_path[0] ? c->env_path : "(없음)", c->inst);
    } else if (!c->inst[0]) {
        snprintf(c->inst, sizeof c->inst, "MOCK200F");
    }

    /* 결과 폴더 */
    char dir[600];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    mkdir(c->out_dir, 0755);
    snprintf(dir, sizeof dir, "%s/%04d%02d%02d_%02d%02d%02d_%s%s", c->out_dir, tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, c->mode_name, c->rl_enabled ? "" : "_norl");
    mkdir(dir, 0755);

    /* 1) 공유메모리 + 프로세스 간 named 세마포 */
    if (create_shm(e) || setup_ipc(e)) {
        perror("IPC setup");
        return 1;
    }
    if (!atomic_is_lock_free(&e->shm->stop) || !atomic_is_lock_free(&e->shm->rl_md.deadline[0]))
        ig_log("main", "경고: 원자 변수가 lock-free가 아님 → 프로세스 간 공유 불가 플랫폼");

    /* 2) 프로세스 내부 동기화 객체 */
    if (rl_init(&e->rl_trade, c->rl_trade_cap, c->rl_hold_ns, c->rl_enabled) ||
        bbuf_init(&e->order_q, 64, sizeof(order_req_t)) || bbuf_init(&e->exch_q, 256, sizeof(exch_order_t)) ||
        reg_init(&e->reg) || book_init(&e->book, c->nstrat) || isem_init_private(&e->exposure, (unsigned)c->max_net) ||
        isem_init_private(&e->csv_mtx, 1) || isem_init_private(&e->ledger_mtx, 1)) {
        perror("sync init");
        return 1;
    }
    static const int windows[] = {3, 5, 8, 4, 6, 10, 7, 12, 3, 5, 8, 4, 6, 10, 7, 12};
    for (int i = 0; i < c->nstrat; i++) {
        if (inbox_init(&e->inbox[i])) return 1;
        e->strat[i] = strategy_new(e, i, windows[i]);
        if (!e->strat[i]) return 1;
    }
    for (int i = 0; i < c->nworkers; i++) {
        samples_init(&e->gw_wait[i], 50000);
        samples_init(&e->gw_place[i], 50000);
    }
    char tpath[700];
    snprintf(tpath, sizeof tpath, "%s/trades.csv", dir);
    e->trades_csv = fopen(tpath, "w");
    if (e->trades_csv) fprintf(e->trades_csv, "wall_ms,strategy,side,qty,price,reason,odno,quote_to_ack_us\n");

    /* 3) 시그널: 스레드를 만들기 전에 막아 둔다.
     * SIGCHLD는 기본 동작이 "무시"라서, macOS 커널은 막혀 있어도 발생 즉시 버린다(sigwait에 안 옴).
     * 빈 핸들러를 달아 기본 동작을 바꿔야 pending으로 남아 sigwait가 받는다. */
    struct sigaction sca;
    memset(&sca, 0, sizeof sca);
    sca.sa_handler = on_sigchld_noop;
    sigemptyset(&sca.sa_mask);
    sca.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sca, NULL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* 4) feeder 프로세스 */
    e->feeder_pid = spawn_feeder(e);
    if (e->feeder_pid < 0) return 1;

    /* 5) KIS 인증 (토큰을 공유메모리에 쓰고 tok_ready로 feeder를 깨운다) */
    if (c->price_src == SRC_KIS || c->exec_src == SRC_KIS) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        if (kis_init_from_env(&e->kis, &e->shm->tok, c->inst)) {
            ig_log("main", "VPS_KIS_APP_KEY / VPS_KIS_APP_SECRET 없음");
            kill(e->feeder_pid, SIGTERM);
            return 1;
        }
        char err[256];
        if (kis_authenticate(&e->kis, err, sizeof err) != KIS_OK) {
            ig_log("main", "KIS 인증 실패: %s", err);
            kill(e->feeder_pid, SIGTERM);
            waitpid(e->feeder_pid, NULL, 0);
            return 1;
        }
        ig_log("main", "KIS 토큰 발급 완료 → tok_ready V()");
    }
    isem_post(&e->tok_ready);

    /* 6) 엔진 스레드 */
    e->t_start_ns = ig_now_ns();
    if (engine_start_threads(e)) {
        perror("pthread_create");
        return 1;
    }
    ig_log("main", "가동: 모드 %s, feeder pid %d, 전략 %d, 워커 %d, 레이트리미터 %s, 결과 %s", c->mode_name,
           (int)e->feeder_pid, c->nstrat, c->nworkers, c->rl_enabled ? "ON" : "OFF", dir);
    if (c->duration_s > 0) alarm((unsigned)c->duration_s);

    /* 7) supervisor 루프 */
    for (;;) {
        int sig = 0;
        sigwait(&set, &sig);
        if (sig == SIGCHLD) {
            int st;
            pid_t p = waitpid(e->feeder_pid, &st, WNOHANG);
            if (p == e->feeder_pid) {
                int reclaimed = rl_reclaim_held(&e->rl_md);
                ig_log("supervisor", "feeder(pid %d) 종료 감지 (%s %d) → permit %d개 회수, 재기동", (int)p,
                       WIFSIGNALED(st) ? "signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st),
                       reclaimed);
                e->feeder_pid = spawn_feeder(e);
                if (e->feeder_pid < 0) break;
            }
            continue;
        }
        ig_log("main", "%s 수신 → 종료 절차 시작", sig == SIGALRM ? "SIGALRM(실행시간 종료)" : "종료 시그널");
        break;
    }

    /* 8) 종료: feeder → md 링 poison → 엔진 스레드들 */
    atomic_store(&e->shm->stop, 1);
    if (e->feeder_pid > 0) {
        kill(e->feeder_pid, SIGTERM);
        int st;
        for (int i = 0; i < 300 && waitpid(e->feeder_pid, &st, WNOHANG) == 0; i++) ig_sleep_ns(10000000);
        if (waitpid(e->feeder_pid, &st, WNOHANG) == 0) {
            kill(e->feeder_pid, SIGKILL);
            waitpid(e->feeder_pid, &st, 0);
        }
    }
    quote_t poison;
    memset(&poison, 0, sizeof poison);
    poison.seq = QUOTE_SEQ_POISON;
    bbuf_put(&e->md_ring, &poison); /* feeder가 사라졌으니 이제 엔진이 유일한 생산자 → SPSC 유지 */
    engine_shutdown(e);
    double run_s = (ig_now_ns() - e->t_start_ns) / 1e9;
    if (e->trades_csv) fclose(e->trades_csv);

    int fails = report(e, dir, run_s);

    /* 9) 정리: named 객체 unlink */
    bbuf_destroy(&e->md_ring);
    rl_destroy(&e->rl_md);
    isem_close(&e->tok_ready);
    munmap(e->shm, e->shm_size);
    shm_unlink(e->shm_name);
    for (int i = 0; i < c->nstrat; i++) {
        strategy_free(e->strat[i]);
        inbox_destroy(&e->inbox[i]);
    }
    return fails ? 3 : 0;
}
