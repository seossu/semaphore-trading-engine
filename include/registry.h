/* 주문 레지스트리: 브로커 주문번호(ODNO) → (어느 전략의 어느 주문인가).
 *
 * 체결 통보는 ODNO만 들고 온다(실제 KIS 체결통보도 그렇다). 그래서 체결이 오면 이 표를 보고
 * 해당 전략 우편함으로 보낸다. 게이트웨이 워커 여러 개(쓰기)와 거래소 체결 스레드(읽기+삭제)가
 * 동시에 만지므로 이진 세마포 mtx로 보호한다.
 *
 * 경쟁 조건(race) 하나를 여기서 처리한다:
 *   워커가 place_order()로 ODNO를 받은 뒤 이 표에 등록(bind)하기 "전에" 체결이 먼저 도착할 수 있다.
 *   (거래소는 주문을 받는 즉시 처리를 시작하므로)
 *   → 조회에 실패한 체결은 버리지 않고 park(보관)해 둔다.
 *   → 워커가 bind할 때 같은 락 안에서 park 목록을 확인해서 넘겨받는다.
 *   등록과 park 확인이 같은 임계구역 안에 있으므로 "둘 다 상대를 못 보고 지나치는" 경우가 없다
 *   (lost update 방지). 이 경로를 실제로 타는지 보려고 --fast-fill 옵션으로 즉시 체결 비율을 올릴 수 있다.
 *
 * 순서 보장: bind할 때 RECEIVED ACK를 락 안에서 우편함에 넣는다. 그래서 같은 주문의 FILLED가
 *   RECEIVED보다 먼저 전략에 도착하는 일이 없다. 락을 쥔 채 put해도 되는 이유는 inbox.h의 용량 논증 참고.
 */
#ifndef IG_REGISTRY_H
#define IG_REGISTRY_H

#include "events.h"
#include "sem.h"

#define REG_CAP 4096
#define REG_PARK 256

typedef struct {
    char odno[IG_ODNO_LEN];
    uint64_t cid;
    int strat;
    int64_t t_quote_ns, t_decide_ns;
    int used; /* 0=빈칸, 1=사용중, 2=삭제됨(tombstone) */
} reg_entry_t;

typedef struct {
    isem_t mtx;
    reg_entry_t e[REG_CAP];
    ack_t park[REG_PARK];
    int npark;
    long parked_total, park_overflow, live;
} registry_t;

int reg_init(registry_t *r);
void reg_destroy(registry_t *r);

/* ent를 등록하고, 같은 임계구역 안에서 in_lock(arg)를 호출한다(RECEIVED 전달용).
 * 이미 park된 체결이 있으면 out에 담아 돌려준다(엔트리는 바로 삭제). 반환값 = 넘겨준 체결 수 */
int reg_bind(registry_t *r, const reg_entry_t *ent, void (*in_lock)(void *), void *arg, ack_t *out,
             int outcap);
/* 체결 ack(odno 채워짐)의 주인을 찾아 cid/strat/시각을 채우고 엔트리를 삭제한다. 1=찾음, 0=park함 */
int reg_route(registry_t *r, ack_t *fill);
int reg_parked_now(registry_t *r);

#endif
