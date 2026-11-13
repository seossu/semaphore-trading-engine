/* 우편함(세 큐 + 카운팅 세마포 items) 테스트.
 *  1) 우선순위: md, ctl, ack 순서로 넣어도 ack → ctl → md 순으로 나온다.
 *  2) 동시성: ack 생산자 2개, ctl 생산자 1개, md 생산자 2개가 동시에 넣는다.
 *     ack/ctl은 하나도 유실되면 안 되고, md는 (받은 수 + 버린 수 = 보낸 수)여야 한다.
 */
#include <pthread.h>
#include <string.h>

#include "common.h"
#include "inbox.h"
#include "test_util.h"

#define N_ACK 20000
#define N_CTL 5000
#define N_MD 50000

static inbox_t IB;
static atomic_long md_sent;
static atomic_int acks_outstanding;

static void *ack_prod(void *x) {
    (void)x;
    for (int i = 0; i < N_ACK; i++) {
        /* 실제 엔진처럼 미소비 ACK 수를 제한한다(inflight 세마포의 역할) */
        while (atomic_fetch_add(&acks_outstanding, 1) >= INBOX_ACK_CAP) { /* 예약 후 확인(TOCTOU 방지) */
            atomic_fetch_sub(&acks_outstanding, 1);
            ig_sleep_ns(10000);
        }
        ack_t a;
        memset(&a, 0, sizeof a);
        a.cid = (uint64_t)i;
        inbox_put_ack(&IB, &a);
    }
    return NULL;
}

static void *ctl_prod(void *x) {
    (void)x;
    event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_CLOCK;
    for (int i = 0; i < N_CTL; i++) inbox_put_ctl(&IB, &ev);
    return NULL;
}

static void *md_prod(void *x) {
    (void)x;
    quote_t q;
    memset(&q, 0, sizeof q);
    for (int i = 0; i < N_MD; i++) {
        inbox_offer_quote(&IB, &q);
        atomic_fetch_add(&md_sent, 1);
    }
    return NULL;
}

int main(void) {
    CHECK(inbox_init(&IB) == 0, "init");

    /* 1) 우선순위 (단일 스레드, 결정적) */
    quote_t q;
    memset(&q, 0, sizeof q);
    event_t c;
    memset(&c, 0, sizeof c);
    c.type = EV_CLOCK;
    ack_t a;
    memset(&a, 0, sizeof a);
    inbox_offer_quote(&IB, &q);
    inbox_put_ctl(&IB, &c);
    inbox_put_ack(&IB, &a);
    event_t ev;
    inbox_get(&IB, &ev);
    CHECK(ev.type == EV_ACK, "첫 이벤트가 ACK가 아님");
    inbox_get(&IB, &ev);
    CHECK(ev.type == EV_CLOCK, "둘째가 CTL이 아님");
    inbox_get(&IB, &ev);
    CHECK(ev.type == EV_QUOTE, "셋째가 MD가 아님");
    PASS("우선순위 ACK > CTL > MD");

    /* 2) 동시성 */
    pthread_t t[5];
    pthread_create(&t[0], NULL, ack_prod, NULL);
    pthread_create(&t[1], NULL, ack_prod, NULL);
    pthread_create(&t[2], NULL, ctl_prod, NULL);
    pthread_create(&t[3], NULL, md_prod, NULL);
    pthread_create(&t[4], NULL, md_prod, NULL);
    long got_ack = 0, got_ctl = 0, got_md = 0;
    long total_needed_ack = 2L * N_ACK, total_ctl = N_CTL;
    while (got_ack < total_needed_ack || got_ctl < total_ctl || atomic_load(&md_sent) < 2L * N_MD ||
           got_md + atomic_load(&IB.md_dropped) < 2L * N_MD) {
        inbox_get(&IB, &ev);
        if (ev.type == EV_ACK) {
            got_ack++;
            atomic_fetch_sub(&acks_outstanding, 1);
        } else if (ev.type == EV_CLOCK) got_ctl++;
        else got_md++;
        if (got_md % 97 == 0) ig_sleep_ns(20000); /* 일부러 느린 소비자 → md 드롭 유발 */
    }
    for (int i = 0; i < 5; i++) pthread_join(t[i], NULL);
    CHECK(got_ack == total_needed_ack, "ACK 유실 %ld", total_needed_ack - got_ack);
    CHECK(got_ctl == total_ctl, "CTL 유실");
    long dropped = atomic_load(&IB.md_dropped);
    CHECK(got_md + dropped == 2L * N_MD, "md 계수 불일치 %ld+%ld", got_md, dropped);
    CHECK(isem_count_slow(&IB.items) == 0, "items 세마포가 0이 아님");
    PASS("생산자 5개 동시: ACK %ld/%ld, CTL %ld/%ld 유실 0, 시세 받음 %ld + 버림 %ld = 보냄 %d", got_ack,
         total_needed_ack, got_ctl, total_ctl, got_md, dropped, 2 * N_MD);
    inbox_destroy(&IB);
    return 0;
}
