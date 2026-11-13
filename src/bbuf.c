#include "bbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t bbuf_state_size(size_t cap, size_t esz) { return sizeof(bbuf_state_t) + cap * esz; }

static void state_init(bbuf_state_t *st, size_t cap, size_t esz, int spsc) {
    st->cap = cap;
    st->esz = esz;
    st->head = st->tail = 0;
    st->spsc = spsc;
}

int bbuf_init(bbuf_t *b, size_t cap, size_t esz) {
    memset(b, 0, sizeof *b);
    if (cap == 0 || cap > 32767) return -1; /* macOS SEM_VALUE_MAX = 32767 */
    b->st = malloc(bbuf_state_size(cap, esz));
    if (!b->st) return -1;
    b->own_state = 1;
    state_init(b->st, cap, esz, 0);
    if (isem_init_private(&b->empty, (unsigned)cap) || isem_init_private(&b->full, 0) ||
        isem_init_private(&b->pmtx, 1) || isem_init_private(&b->cmtx, 1))
        return -1;
    return 0;
}

int bbuf_create_shared(bbuf_t *b, void *mem, size_t cap, size_t esz, const char *prefix) {
    memset(b, 0, sizeof *b);
    if (cap == 0 || cap > 32767) return -1;
    b->st = mem;
    state_init(b->st, cap, esz, 1);
    char n[32];
    snprintf(n, sizeof n, "%s.e", prefix);
    if (isem_create_named(&b->empty, n, (unsigned)cap)) return -1;
    snprintf(n, sizeof n, "%s.f", prefix);
    if (isem_create_named(&b->full, n, 0)) return -1;
    return 0;
}

int bbuf_attach_shared(bbuf_t *b, void *mem, const char *prefix) {
    memset(b, 0, sizeof *b);
    b->st = mem;
    char n[32];
    snprintf(n, sizeof n, "%s.e", prefix);
    if (isem_open_named(&b->empty, n)) return -1;
    snprintf(n, sizeof n, "%s.f", prefix);
    if (isem_open_named(&b->full, n)) return -1;
    return 0;
}

void bbuf_destroy(bbuf_t *b) {
    isem_close(&b->empty);
    isem_close(&b->full);
    isem_close(&b->pmtx);
    isem_close(&b->cmtx);
    if (b->own_state) free(b->st);
    b->st = NULL;
}

/* 아래 두 함수는 empty/full로 "이 칸은 지금 나만 만진다"가 보장된 뒤에만 호출된다. */
static void slot_write(bbuf_state_t *st, const void *e) {
    memcpy(st->data + st->tail * st->esz, e, st->esz);
    st->tail = (st->tail + 1) % st->cap;
}

static void slot_read(bbuf_state_t *st, void *out) {
    memcpy(out, st->data + st->head * st->esz, st->esz);
    st->head = (st->head + 1) % st->cap;
}

static void put_locked(bbuf_t *b, const void *e) {
#ifdef IG_BUG_NO_PMTX
    int mpmc = 0; /* 음성 대조군: 생산자 mutex를 일부러 뺀다 → TSan이 잡아야 정상 (make tsan-negative) */
#else
    int mpmc = !b->st->spsc;
#endif
    if (mpmc) isem_wait(&b->pmtx);
    slot_write(b->st, e);
    if (mpmc) isem_post(&b->pmtx);
    isem_post(&b->full); /* 데이터를 다 쓴 뒤에 알린다 */
}

static void get_locked(bbuf_t *b, void *out) {
    int mpmc = !b->st->spsc;
    if (mpmc) isem_wait(&b->cmtx);
    slot_read(b->st, out);
    if (mpmc) isem_post(&b->cmtx);
    isem_post(&b->empty); /* 다 읽은 뒤에야 칸을 돌려준다 */
}

void bbuf_put(bbuf_t *b, const void *e) {
    isem_wait(&b->empty); /* ① 빈 칸 확보 — 반드시 mutex보다 먼저 */
    put_locked(b, e);     /* ② mutex 안에서 기록 → ③ V(full) */
}

int bbuf_tryput(bbuf_t *b, const void *e) {
    if (!isem_trywait(&b->empty)) return 0;
    put_locked(b, e);
    return 1;
}

void bbuf_get(bbuf_t *b, void *out) {
    isem_wait(&b->full);
    get_locked(b, out);
}

int bbuf_tryget(bbuf_t *b, void *out) {
    if (!isem_trywait(&b->full)) return 0;
    get_locked(b, out);
    return 1;
}
