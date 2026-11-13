#include "sem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

/* ThreadSanitizer 주석.
 * macOS의 TSan 런타임은 pthread_mutex는 가로채지만 sem_wait/sem_post는 가로채지 않는다
 * (libclang_rt.tsan_osx_dynamic.dylib에 _wrap_sem_wait가 없음). 그러면 세마포로 보호된 정상 코드도
 * 데이터 경쟁으로 보고된다. POSIX는 sem_post → (그 값을 얻은) sem_wait 사이에 메모리 동기화를
 * 보장하므로, 그 관계를 TSan에 그대로 알려준다: post 직전 release, wait 성공 직후 acquire.
 * 일반 빌드에서는 아무 코드도 생성되지 않는다. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#define TSAN_RELEASE(p) __tsan_release(p)
#define TSAN_ACQUIRE(p) __tsan_acquire(p)
#endif
#endif
#ifndef TSAN_RELEASE
#define TSAN_RELEASE(p) ((void)0)
#define TSAN_ACQUIRE(p) ((void)0)
#endif

static atomic_uint g_seq;

static sem_t *open_new(const char *name, unsigned value) {
    sem_t *h = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    if (h == SEM_FAILED && errno == EEXIST) {
        /* 이전 실행이 비정상 종료하면서 남긴 같은 이름이 있으면 지우고 새로 만든다 */
        sem_unlink(name);
        h = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    }
    return h;
}

int isem_init_private(isem_t *s, unsigned value) {
    char name[32];
    snprintf(name, sizeof name, "/igp%d.%u", (int)getpid(), atomic_fetch_add(&g_seq, 1u));
    sem_t *h = open_new(name, value);
    if (h == SEM_FAILED) return -1;
    sem_unlink(name); /* 이름 즉시 삭제 → 핸들만 남은 익명 세마포 */
    s->h = h;
    s->name[0] = '\0';
    s->owner = 0;
    return 0;
}

int isem_create_named(isem_t *s, const char *name, unsigned value) {
    sem_t *h = open_new(name, value);
    if (h == SEM_FAILED) return -1;
    s->h = h;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->owner = 1;
    return 0;
}

int isem_open_named(isem_t *s, const char *name) {
    sem_t *h = sem_open(name, 0);
    if (h == SEM_FAILED) return -1;
    s->h = h;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->owner = 0;
    return 0;
}

void isem_close(isem_t *s) {
    if (!s->h) return;
    sem_close(s->h);
    if (s->owner && s->name[0]) sem_unlink(s->name);
    s->h = NULL;
}

void isem_wait(isem_t *s) {
    while (sem_wait(s->h) == -1) {
        if (errno == EINTR) continue;
        ig_die("sem_wait: %s", strerror(errno));
    }
    TSAN_ACQUIRE(s->h);
}

int isem_wait_intr(isem_t *s, const atomic_int *stop) {
    for (;;) {
        if (sem_wait(s->h) == 0) {
            TSAN_ACQUIRE(s->h);
            return 0;
        }
        if (errno != EINTR) ig_die("sem_wait: %s", strerror(errno));
        if (stop && atomic_load(stop)) return -1;
    }
}

int isem_trywait(isem_t *s) {
    for (;;) {
        if (sem_trywait(s->h) == 0) {
            TSAN_ACQUIRE(s->h);
            return 1;
        }
        if (errno == EAGAIN) return 0;
        if (errno == EINTR) continue;
        ig_die("sem_trywait: %s", strerror(errno));
    }
}

void isem_post(isem_t *s) {
    TSAN_RELEASE(s->h);
    if (sem_post(s->h) == -1) ig_die("sem_post: %s", strerror(errno));
}

int isem_count_slow(isem_t *s) {
    int n = 0;
    while (isem_trywait(s)) n++;
    for (int i = 0; i < n; i++) isem_post(s);
    return n;
}
