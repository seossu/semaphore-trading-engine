#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int64_t ig_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t ig_wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

void ig_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    struct timespec ts = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    /* 시그널로 깨도 남은 시간만큼 마저 잔다. 중간 종료가 필요하면 ig_sleep_until_stop. */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

void ig_sleep_until_stop(int64_t ns, const atomic_int *stop) {
    int64_t end = ig_now_ns() + ns;
    while (!atomic_load(stop)) {
        int64_t left = end - ig_now_ns();
        if (left <= 0) break;
        struct timespec ts = {0, (long)(left < 20000000 ? left : 20000000)};
        nanosleep(&ts, NULL); /* EINTR이면 그냥 루프로 돌아가 stop을 다시 본다 */
    }
}

int ig_local_hhmmss(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 10000 + tm.tm_min * 100 + tm.tm_sec;
}

static const char *g_role = "engine";
void ig_set_role(const char *role) { g_role = role; }

void ig_log(const char *tag, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int64_t ms = ig_wall_ms();
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&s, &tm);
    /* fprintf 한 번 = 한 줄. stdio가 FILE 단위 락을 잡으므로 여러 스레드가 동시에 써도
     * 줄이 섞이지 않는다(이건 libc가 제공하는 동기화라 따로 세마포를 두지 않았다). */
    fprintf(stderr, "%02d:%02d:%02d.%03d %-6s [%s] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
            (int)(ms % 1000), g_role, tag, msg);
}

void ig_die(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL [%s] %s\n", g_role, msg);
    abort();
}

uint64_t ig_rand_next(uint64_t *s) {
    uint64_t x = *s ? *s : 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

double ig_rand_unit(uint64_t *s) { return (double)(ig_rand_next(s) >> 11) * (1.0 / 9007199254740992.0); }

double ig_rand_normal(uint64_t *s) {
    double u1 = ig_rand_unit(s), u2 = ig_rand_unit(s);
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

int ig_load_env_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[2048];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *t = trim(line);
        if (*t == '\0' || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(t), *v = trim(eq + 1);
        size_t vl = strlen(v);
        if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) {
            v[vl - 1] = '\0';
            v++;
        }
        if (*k) {
            setenv(k, v, 0);
            n++;
        }
    }
    fclose(f);
    return n;
}

const char *ig_getenv(const char *key, const char *def) {
    const char *v = getenv(key);
    return (v && *v) ? v : def;
}

int samples_init(samples_t *s, size_t cap) {
    s->v = malloc(cap * sizeof *s->v);
    s->n = 0;
    s->cap = s->v ? cap : 0;
    return s->v ? 0 : -1;
}

void samples_push(samples_t *s, int64_t x) {
    if (s->n < s->cap) s->v[s->n++] = x;
}

void samples_free(samples_t *s) {
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

void samples_merge(samples_t *dst, const samples_t *src) {
    for (size_t i = 0; i < src->n; i++) samples_push(dst, src->v[i]);
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

int64_t samples_pct(samples_t *s, double p) {
    if (s->n == 0) return -1;
    qsort(s->v, s->n, sizeof *s->v, cmp_i64);
    size_t idx = (size_t)(p / 100.0 * (double)(s->n - 1) + 0.5);
    if (idx >= s->n) idx = s->n - 1;
    return s->v[idx];
}
