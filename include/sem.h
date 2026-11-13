/* POSIX named 세마포 래퍼.
 *
 * 왜 sem_open(named)인가:
 *   macOS는 이름 없는 세마포(sem_init)를 지원하지 않는다(호출하면 ENOSYS로 실패).
 *   그래서 이 프로젝트의 세마포는 전부 sem_open으로 만든다. 두 가지 용도로 쓴다.
 *
 *   1) 프로세스 내부용(isem_init_private)
 *      고유 이름으로 만든 직후 sem_unlink 한다. 이름은 사라지고 핸들만 남아서
 *      사실상 "익명 세마포"처럼 동작한다. 프로세스가 비정상 종료해도 커널 객체가 남지 않는다.
 *
 *   2) 프로세스 간 공유용(isem_create_named / isem_open_named)
 *      엔진 프로세스가 이름으로 만들고, posix_spawn으로 띄운 feeder 프로세스가 같은 이름으로 연다.
 *      fork 상속이 아니라 이름으로 붙기 때문에 feeder가 죽어도 엔진이 새로 띄워서 다시 붙일 수 있다.
 *      이름은 만든 쪽(owner)이 종료할 때 sem_unlink 한다.
 *
 * 참고: macOS는 sem_timedwait / sem_getvalue도 지원하지 않는다. 그래서
 *   - 타임아웃 대기 대신 "시그널(EINTR) + stop 플래그"로 깨운다(isem_wait_intr).
 *   - 남은 값 확인은 trywait로 빼면서 세는 방식으로 한다(테스트/종료 검증에서만).
 */
#ifndef IG_SEM_H
#define IG_SEM_H

#include <semaphore.h>
#include <stdatomic.h>

typedef struct {
    sem_t *h;
    char name[32];
    int owner; /* 1이면 close할 때 sem_unlink까지 */
} isem_t;

int isem_init_private(isem_t *s, unsigned value);
int isem_create_named(isem_t *s, const char *name, unsigned value);
int isem_open_named(isem_t *s, const char *name);
void isem_close(isem_t *s);

void isem_wait(isem_t *s);                          /* P(): EINTR이면 다시 기다림 */
int isem_wait_intr(isem_t *s, const atomic_int *stop); /* 0=획득, -1=시그널로 깼고 stop이 켜짐 */
int isem_trywait(isem_t *s);                        /* 1=획득, 0=값이 0 */
void isem_post(isem_t *s);                          /* V() */

/* 값을 0이 될 때까지 trywait로 빼서 센다. 종료 후 불변식 검증용(뺀 만큼 다시 post한다). */
int isem_count_slow(isem_t *s);

#endif
