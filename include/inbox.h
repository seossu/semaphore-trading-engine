/* 전략 스레드 하나의 우편함: 우선순위가 다른 큐 3개 + 그 셋을 함께 세는 카운팅 세마포 1개.
 *
 *   ack : 내 주문의 접수/체결/거절. 절대 버리면 안 됨(장부 정합성).
 *   ctl : 시계 틱, 전량청산(FLATTEN), 종료(SHUTDOWN).
 *   md  : 시세. 전략이 느리면 버려도 된다(오래된 시세는 어차피 가치가 없다) → 버린 수를 센다.
 *   items : 세 큐에 들어 있는 이벤트 총수.
 *
 * 소비자(전략 스레드 1개)는 P(items)로 "뭔가 하나는 있다"를 확인한 뒤 ack → ctl → md 순으로
 * tryget 한다. 생산자는 항상 큐에 넣은 다음에 V(items)를 하므로, P(items)가 통과하면
 * 세 큐 중 하나에는 반드시 원소가 있다(소비자가 하나뿐이라 다른 누가 가져갈 수도 없다).
 * 이렇게 하면 "여러 큐 중 아무거나 오면 깨우기"를 select나 조건변수 없이 세마포 하나로 할 수 있다.
 *
 * ACK를 먼저 처리하는 이유: 체결을 반영하기 전에 새 시세로 판단하면, 이미 들어간 포지션을 모르고
 * 중복 진입할 수 있다.
 *
 * ack 큐가 꽉 차지 않는다는 증명(docs/DESIGN.md):
 *   전략마다 in-flight 주문 수를 세마포 inflight(초기값 K)로 제한한다. 주문 하나가 만드는 ACK는
 *   최대 2개(RECEIVED+FILLED, 또는 REJECTED 1개)이고, permit은 마지막 ACK를 "소비"한 뒤에야
 *   반납된다. 그래서 아직 소비되지 않은 ACK는 2K개 이하다. ACK_CAP ≥ 2K이면 ack put은 절대 막히지 않는다.
 *   그래서 put_ack는 registry 락을 쥔 채 호출해도 교착이 생기지 않는다.
 */
#ifndef IG_INBOX_H
#define IG_INBOX_H

#include <stdatomic.h>

#include "bbuf.h"
#include "events.h"

#define INBOX_ACK_CAP 64
#define INBOX_CTL_CAP 64
#define INBOX_MD_CAP 128

typedef struct {
    bbuf_t ack, ctl, md;
    isem_t items;
    atomic_long md_dropped, ctl_dropped, ack_overflow;
} inbox_t;

int inbox_init(inbox_t *ib);
void inbox_destroy(inbox_t *ib);

void inbox_put_ack(inbox_t *ib, const ack_t *a);        /* 증명상 절대 막히지 않음. 넘치면 즉시 abort */
void inbox_put_ctl(inbox_t *ib, const event_t *ev);     /* 블로킹 */
int inbox_tryput_ctl(inbox_t *ib, const event_t *ev);   /* 시계 틱용, 꽉 차면 버림 */
int inbox_offer_quote(inbox_t *ib, const quote_t *q);   /* 꽉 차면 버리고 0 */
void inbox_get(inbox_t *ib, event_t *out);              /* ACK > CTL > MD */

#endif
