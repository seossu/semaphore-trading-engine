/* 모의 KIS 서버의 "초당 요청 제한" 판정기.
 *
 * 실제 KIS처럼 최근 1초 동안 들어온 요청이 limit건 이상이면 EGW00201로 거절한다.
 * feeder 프로세스와 엔진 프로세스가 같은 app key를 쓰는 상황을 흉내 내야 하므로 공유메모리에 둔다.
 *
 * 100ms 버킷 10개(현재 버킷 + 직전 9개)를 합산한다. 이 합은 "최근 1.0초 이내" 요청만 센다
 * (실제 1초 슬라이딩 윈도보다 덜 세면 덜 셌지 더 세지는 않는다). 그래서 rate limiter가
 * 제한을 지키는데도 모의 서버가 억울하게 거절하는 일은 없다.
 * 여러 프로세스가 동시에 판정하므로 mutex 없이 원자 연산만 쓴다(죽은 프로세스가 락을 남기지 않게).
 */
#ifndef IG_MOCK_SERVER_H
#define IG_MOCK_SERVER_H

#include <stdatomic.h>

#define MS_BUCKETS 10
#define MS_BUCKET_MS 100

typedef struct {
    int limit; /* 초당 허용 건수. 0이면 무제한 */
    atomic_llong epoch[MS_BUCKETS];
    atomic_int count[MS_BUCKETS];
    atomic_long admitted, rejected;
} mock_server_t;

void mock_server_init(mock_server_t *ms, int limit);
int mock_server_admit(mock_server_t *ms); /* 1=허용, 0=EGW00201 */

#endif
