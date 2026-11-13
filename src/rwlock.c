#include "rwlock.h"

int rw_init(rwlock_t *l) {
    l->readers = 0;
    if (isem_init_private(&l->turnstile, 1) || isem_init_private(&l->room_empty, 1) ||
        isem_init_private(&l->rmtx, 1))
        return -1;
    return 0;
}

void rw_destroy(rwlock_t *l) {
    isem_close(&l->turnstile);
    isem_close(&l->room_empty);
    isem_close(&l->rmtx);
}

void rw_rdlock(rwlock_t *l) {
    /* 회전문을 통과만 한다(잡았다 바로 놓음). writer가 회전문을 쥐고 있으면 여기서 기다린다. */
    isem_wait(&l->turnstile);
    isem_post(&l->turnstile);

    isem_wait(&l->rmtx);
    if (++l->readers == 1) isem_wait(&l->room_empty); /* 첫 reader가 방을 잠근다 */
    isem_post(&l->rmtx);
}

void rw_rdunlock(rwlock_t *l) {
    isem_wait(&l->rmtx);
    if (--l->readers == 0) isem_post(&l->room_empty); /* 마지막 reader가 방을 연다 */
    isem_post(&l->rmtx);
}

void rw_wrlock(rwlock_t *l) {
    isem_wait(&l->turnstile);  /* 새 reader 유입 차단 */
    isem_wait(&l->room_empty); /* 이미 들어간 reader들이 다 나갈 때까지 대기 */
}

void rw_wrunlock(rwlock_t *l) {
    isem_post(&l->turnstile);
    isem_post(&l->room_empty);
}
