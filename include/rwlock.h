/* 세마포만으로 만든 Readers-Writers 락 (쓰기 스레드가 굶지 않는 버전).
 *
 * Allen B. Downey, "The Little Book of Semaphores" 4.2.6 "No-starve readers-writers".
 *
 *   turnstile  : 회전문. writer가 도착하면 여기를 잠가서 "새로 오는" reader를 막는다.
 *   room_empty : 방에 아무도 없음(1) / 누군가 있음(0)
 *   rmtx       : readers 카운터 보호
 *
 * 왜 필요한가: 포지션 장부(book)는 전략 스레드 여러 개와 모니터가 자주 읽고,
 * 체결 처리와 잔고 폴러만 가끔 쓴다. 단순한 "첫 reader가 잠그고 마지막 reader가 푸는"
 * 방식(lightswitch)만 쓰면, reader가 끊임없이 들어올 때 writer(체결 반영)가 영원히 못 들어간다.
 * 체결 반영이 늦어지면 장부가 실제 계좌와 어긋나므로 writer 기아(starvation)를 막아야 한다.
 */
#ifndef IG_RWLOCK_H
#define IG_RWLOCK_H

#include "sem.h"

typedef struct {
    isem_t turnstile, room_empty, rmtx;
    int readers;
} rwlock_t;

int rw_init(rwlock_t *l);
void rw_destroy(rwlock_t *l);
void rw_rdlock(rwlock_t *l);
void rw_rdunlock(rwlock_t *l);
void rw_wrlock(rwlock_t *l);
void rw_wrunlock(rwlock_t *l);

#endif
