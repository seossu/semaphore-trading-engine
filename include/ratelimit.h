/* API 호출 속도 제한기 — "1초 동안 최대 C건"을 카운팅 세마포 하나로 보장한다.
 *
 * 문제: KIS는 초당 요청 수를 넘기면 EGW00201(초당 거래건수 초과)로 거절한다.
 *
 * 해법: 세마포 permits = C (초기값). 요청 하나 = permit 하나.
 *   - 요청 전에 P(permits)
 *   - 응답을 받은 시각 + window(1초) 뒤에야 V(permits)로 돌려준다(refill 스레드가 담당).
 *
 * 보장: 어떤 permit이 다음 요청에 쓰이려면, 그 permit의 이전 요청 응답 시각부터 1초가 지나야 한다.
 *   서버 도착 시각 ≤ 응답 시각이므로, 서버가 보는 어떤 1초 구간 안에도 같은 permit의 요청은
 *   최대 1건이다. permit이 C개뿐이므로 1초 구간 안의 요청은 최대 C건이다.
 *   (흔한 "1초마다 C개 채워주기" 토큰 버킷은 경계에서 순간적으로 2C건까지 허용할 수 있다.)
 *   부수 효과로 동시에 진행 중인 요청 수도 C개 이하로 제한된다.
 *
 * 프로세스 간 공유: feeder 프로세스(시세 조회)와 엔진(refill)이 같은 permit을 공유하므로,
 *   상태(rl_state_t)는 공유메모리에, 세마포는 named로 만든다.
 *   반납 예약 슬롯(deadline[])은 mutex 대신 CAS로 다룬다. feeder가 mutex를 쥔 채 죽으면
 *   엔진의 refill 스레드까지 영원히 멈추기 때문이다(세마포에는 robust 모드가 없다).
 */
#ifndef IG_RATELIMIT_H
#define IG_RATELIMIT_H

#include <stdatomic.h>
#include <stdint.h>

#include "sem.h"

#define RL_MAX_PERMITS 64

typedef struct {
    int cap;
    int enabled;
    int64_t hold_ns;                           /* 응답 후 permit을 붙잡아 두는 시간 (window + 여유) */
    atomic_llong deadline[RL_MAX_PERMITS];     /* 0 = 빈 슬롯, 그 외 = 이 시각에 V() */
    atomic_long acquired;                      /* 통계: 누적 획득 수 */
    atomic_long held;                          /* 지금 P()만 하고 아직 반납 예약을 안 한 수 */
} rl_state_t;

typedef struct {
    rl_state_t *st;
    int own_state;
    isem_t permits;
} ratelimit_t;

int rl_init(ratelimit_t *rl, int cap, int64_t hold_ns, int enabled);
int rl_create_shared(ratelimit_t *rl, rl_state_t *mem, int cap, int64_t hold_ns, int enabled,
                     const char *sem_name);
int rl_attach_shared(ratelimit_t *rl, rl_state_t *mem, const char *sem_name);
void rl_destroy(ratelimit_t *rl);

void rl_acquire(ratelimit_t *rl);
int rl_acquire_intr(ratelimit_t *rl, const atomic_int *stop); /* -1이면 stop으로 중단 */
void rl_release_deferred(ratelimit_t *rl);                    /* 응답 직후 호출 */

/* 만기된 permit을 V() 한다. 다음 만기까지 남은 ns를 돌려준다(refill 스레드가 그만큼 잔다). */
int64_t rl_refill(ratelimit_t *rl);
/* permit을 쥔 프로세스가 죽었을 때 supervisor가 회수한다. 회수한 개수를 돌려준다. */
int rl_reclaim_held(ratelimit_t *rl);

#endif
