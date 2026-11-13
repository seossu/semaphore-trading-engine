/* 유한 버퍼(bounded buffer) — 고전적인 생산자-소비자 문제의 세마포 해법.
 *
 *   empty : 빈 칸 수 (초기값 cap)   생산자가 P, 소비자가 V
 *   full  : 찬 칸 수 (초기값 0)     소비자가 P, 생산자가 V
 *   pmtx  : 생산자끼리 tail 보호 (이진 세마포)
 *   cmtx  : 소비자끼리 head 보호 (이진 세마포)
 *
 * 순서가 중요하다: put은 반드시 P(empty) → P(pmtx) 순서다.
 *   반대로 P(pmtx)를 먼저 잡고 P(empty)에서 잠들면, 버퍼가 꽉 찬 상태에서 mutex를 쥔 채 자게 된다.
 *   그러면 다른 생산자는 mutex에서 막히고, 소비자가 칸을 비워도 그 생산자를 깨울 수 없다(교착 상태).
 *
 * 생산자 mutex와 소비자 mutex를 분리한 이유:
 *   생산자는 tail만, 소비자는 head만 건드리고, 같은 칸에 동시에 접근하지 않는다는 건
 *   empty/full 카운트가 보장한다. 그래서 하나의 큰 mutex를 둘 필요가 없고, 넣기와 빼기가 병렬로 진행된다.
 *   (증명: docs/DESIGN.md "유한 버퍼 정확성")
 *
 * SPSC 모드(공유메모리, 프로세스 간):
 *   생산자 1개 / 소비자 1개라 pmtx/cmtx가 필요 없다. 그리고 mutex를 안 쓰는 게 더 안전하다.
 *   이진 세마포를 쥔 프로세스가 kill -9 당하면 그 세마포는 영원히 0으로 남는다
 *   (robust mutex 같은 장치가 세마포에는 없다). 반면 카운팅 세마포 empty/full은 그런 상황에서도
 *   "칸 하나가 줄어드는" 정도로만 손상된다.
 */
#ifndef IG_BBUF_H
#define IG_BBUF_H

#include <stddef.h>

#include "sem.h"

typedef struct {
    size_t cap, esz;
    size_t head; /* 소비자만 씀 */
    size_t tail; /* 생산자만 씀 */
    int spsc;
    unsigned char data[];
} bbuf_state_t;

typedef struct {
    bbuf_state_t *st; /* 프로세스 내부면 malloc, 프로세스 간이면 공유메모리 위 */
    int own_state;
    isem_t empty, full, pmtx, cmtx;
} bbuf_t;

size_t bbuf_state_size(size_t cap, size_t esz);

int bbuf_init(bbuf_t *b, size_t cap, size_t esz); /* 프로세스 내부 MPMC */
int bbuf_create_shared(bbuf_t *b, void *mem, size_t cap, size_t esz, const char *prefix); /* SPSC */
int bbuf_attach_shared(bbuf_t *b, void *mem, const char *prefix);
void bbuf_destroy(bbuf_t *b);

void bbuf_put(bbuf_t *b, const void *elem);
int bbuf_tryput(bbuf_t *b, const void *elem); /* 1=넣음, 0=꽉 참 */
void bbuf_get(bbuf_t *b, void *out);
int bbuf_tryget(bbuf_t *b, void *out); /* 1=꺼냄, 0=비어 있음 */

#endif
