/* 한국투자증권(KIS) Open API REST 클라이언트 (libcurl).
 *
 * 토큰 공유: 접근 토큰은 엔진만 발급하고(tokenP는 1분에 1번 제한), 공유메모리(token_state_t)에 둔다.
 *   feeder 프로세스는 읽기만 한다. 갱신은 하루 한두 번뿐이라 이중 버퍼 + 버전 번호로 처리한다
 *   (쓰는 쪽은 안 쓰는 버퍼에 기록한 뒤 버전을 올린다). 프로세스 간 락을 없앤 이유는 ratelimit.h와 같다.
 */
#ifndef IG_KIS_H
#define IG_KIS_H

#include <stdatomic.h>
#include <stdint.h>

#include "events.h"

typedef struct {
    atomic_uint ver; /* 0 = 아직 없음 */
    char tok[2][1024];
    atomic_llong issued_ms;
    atomic_int refresh_req; /* feeder가 토큰 만료(EGW00123)를 보면 1로 올린다 */
} token_state_t;

typedef struct {
    char base[128], key[128], secret[512], cano[16], prdt[8];
    char inst[IG_SYM_LEN];
    token_state_t *tok;
} kis_t;

typedef struct {
    int ok;
    double cash, pnl;
    int net, net_known;
} balance_t;

/* 반환 코드 */
#define KIS_OK 0
#define KIS_ERR -1
#define KIS_RATE_LIMITED -2 /* EGW00201 */
#define KIS_TOKEN_EXPIRED -3 /* EGW00123 */

int kis_init_from_env(kis_t *k, token_state_t *tok, const char *inst);
int kis_authenticate(kis_t *k, char *err, size_t errsz);
int kis_get_price(kis_t *k, double *px, char *err, size_t errsz);
int kis_place(kis_t *k, const order_req_t *r, char *odno, char *msg);
int kis_balance(kis_t *k, balance_t *b, char *err, size_t errsz);

/* http.c / json.c */
typedef struct {
    long status;
    char *body;
    size_t len;
    char err[128];
} http_resp_t;
int http_request(const char *method, const char *url, const char *const *headers, int nh,
                 const char *body, http_resp_t *out);
void http_resp_free(http_resp_t *r);
/* "key":"value" 형태의 첫 번째 문자열 값을 꺼낸다(데모용 최소 파서). 1=찾음 */
int json_str(const char *body, const char *key, char *out, size_t outsz);

#endif
