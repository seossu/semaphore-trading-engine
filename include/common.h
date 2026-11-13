/* 공통 유틸: 시간, 로그, 난수, .env 로딩, 샘플(지연시간) 수집. */
#ifndef IG_COMMON_H
#define IG_COMMON_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IG_MAX_STRAT 16
#define IG_MAX_WORKERS 16
#define IG_SYM_LEN 16
#define IG_ODNO_LEN 24
#define IG_MSG_LEN 96
#define IG_IPC_PREFIX_LEN 20

/* KOSPI200 선물 1pt = 250,000원 */
#define IG_KRW_PER_POINT 250000.0

int64_t ig_now_ns(void);  /* CLOCK_MONOTONIC: 프로세스 사이에서도 같은 기준이라 IPC 지연 측정에 쓴다 */
int64_t ig_wall_ms(void); /* CLOCK_REALTIME: 분봉 경계, 로그 시각 */
void ig_sleep_ns(int64_t ns);
/* stop이 켜지면 일찍 깨는 sleep. 조건변수 없이 20ms 단위로 쪼개서 잔다. */
void ig_sleep_until_stop(int64_t ns, const atomic_int *stop);
int ig_local_hhmmss(void);

void ig_set_role(const char *role);
void ig_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
_Noreturn void ig_die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* xorshift64* (스레드마다 자기 state를 갖고 쓰므로 공유 없음) */
uint64_t ig_rand_next(uint64_t *state);
double ig_rand_unit(uint64_t *state);   /* [0,1) */
double ig_rand_normal(uint64_t *state); /* N(0,1) */

int ig_load_env_file(const char *path); /* KEY=VALUE, 이미 있는 환경변수는 덮어쓰지 않음 */
const char *ig_getenv(const char *key, const char *def);

/* 지연시간 샘플: 스레드마다 하나씩 소유 → 기록할 때 동기화가 필요 없다.
 * 합치는 건 pthread_join 이후(join이 happens-before를 보장). */
typedef struct {
    int64_t *v;
    size_t n, cap;
} samples_t;
int samples_init(samples_t *s, size_t cap);
void samples_push(samples_t *s, int64_t x);
void samples_free(samples_t *s);
void samples_merge(samples_t *dst, const samples_t *src);
/* 정렬 후 백분위. n==0이면 -1 */
int64_t samples_pct(samples_t *s, double p);

#endif
