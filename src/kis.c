#include "kis.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/* ---------------- HTTP (libcurl easy, 호출마다 핸들 생성 → 스레드 간 공유 상태 없음) ---------------- */

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
    http_resp_t *r = ud;
    size_t add = sz * n;
    char *nb = realloc(r->body, r->len + add + 1);
    if (!nb) return 0;
    memcpy(nb + r->len, p, add);
    r->body = nb;
    r->len += add;
    r->body[r->len] = '\0';
    return add;
}

int http_request(const char *method, const char *url, const char *const *headers, int nh,
                 const char *body, http_resp_t *out) {
    memset(out, 0, sizeof *out);
    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(out->err, sizeof out->err, "curl_easy_init failed");
        return -1;
    }
    struct curl_slist *hl = NULL;
    for (int i = 0; i < nh; i++) hl = curl_slist_append(hl, headers[i]);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); /* 멀티스레드에서 SIGALRM 기반 타임아웃 금지 */
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        snprintf(out->err, sizeof out->err, "%s", curl_easy_strerror(rc));
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
    }
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? 0 : -1;
}

void http_resp_free(http_resp_t *r) {
    free(r->body);
    r->body = NULL;
}

int json_str(const char *body, const char *key, char *out, size_t outsz) {
    if (!body) return 0;
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return 1;
}

/* ---------------- KIS ---------------- */

int kis_init_from_env(kis_t *k, token_state_t *tok, const char *inst) {
    memset(k, 0, sizeof *k);
    snprintf(k->base, sizeof k->base, "%s",
             ig_getenv("KIS_BASE_URL", "https://openapivts.koreainvestment.com:29443"));
    snprintf(k->key, sizeof k->key, "%s", ig_getenv("VPS_KIS_APP_KEY", ""));
    snprintf(k->secret, sizeof k->secret, "%s", ig_getenv("VPS_KIS_APP_SECRET", ""));
    const char *acct = ig_getenv("KIS_ACCOUNT_NO", "");
    const char *dash = strchr(acct, '-');
    if (dash) {
        snprintf(k->cano, sizeof k->cano, "%.*s", (int)(dash - acct), acct);
        snprintf(k->prdt, sizeof k->prdt, "%s", dash + 1);
    } else {
        snprintf(k->cano, sizeof k->cano, "%s", acct);
        snprintf(k->prdt, sizeof k->prdt, "03"); /* 선물옵션 계좌 상품코드 기본값 */
    }
    snprintf(k->inst, sizeof k->inst, "%s", inst);
    k->tok = tok;
    return (k->key[0] && k->secret[0]) ? 0 : -1;
}

static void read_token(kis_t *k, char *out, size_t n) {
    unsigned v = atomic_load_explicit(&k->tok->ver, memory_order_acquire);
    if (v == 0) {
        out[0] = '\0';
        return;
    }
    snprintf(out, n, "%s", k->tok->tok[v & 1]);
}

static int classify(const http_resp_t *r, const char *must_have, char *err, size_t errsz) {
    if (r->status == 0) {
        snprintf(err, errsz, "network: %s", r->err);
        return KIS_ERR;
    }
    if (r->body && strstr(r->body, "EGW00201")) {
        snprintf(err, errsz, "EGW00201 초당 거래건수 초과");
        return KIS_RATE_LIMITED;
    }
    if (r->body && strstr(r->body, "EGW00123")) {
        snprintf(err, errsz, "EGW00123 토큰 만료");
        return KIS_TOKEN_EXPIRED;
    }
    char tmp[8];
    if (r->status != 200 || (must_have && !json_str(r->body, must_have, tmp, sizeof tmp))) {
        char m1[160] = "";
        json_str(r->body, "msg1", m1, sizeof m1);
        snprintf(err, errsz, "status=%ld msg1=%s", r->status, m1);
        return KIS_ERR;
    }
    return KIS_OK;
}

int kis_authenticate(kis_t *k, char *err, size_t errsz) {
    char body[1024];
    snprintf(body, sizeof body,
             "{\"grant_type\":\"client_credentials\",\"appkey\":\"%s\",\"appsecret\":\"%s\"}", k->key,
             k->secret);
    const char *h[] = {"Content-Type: application/json; charset=utf-8"};
    char url[256];
    snprintf(url, sizeof url, "%s/oauth2/tokenP", k->base);
    http_resp_t r;
    http_request("POST", url, h, 1, body, &r);
    char tok[1024];
    int rc = KIS_ERR;
    if (r.status == 200 && json_str(r.body, "access_token", tok, sizeof tok)) {
        unsigned v = atomic_load(&k->tok->ver);
        snprintf(k->tok->tok[(v + 1) & 1], sizeof k->tok->tok[0], "%s", tok); /* 안 쓰는 버퍼에 기록 */
        atomic_store(&k->tok->issued_ms, ig_wall_ms());
        atomic_store_explicit(&k->tok->ver, v + 1, memory_order_release); /* 그 다음 공개 */
        atomic_store(&k->tok->refresh_req, 0);
        rc = KIS_OK;
    } else {
        char m1[160] = "";
        json_str(r.body, "error_description", m1, sizeof m1);
        snprintf(err, errsz, "status=%ld %s %s", r.status, r.err, m1);
    }
    http_resp_free(&r);
    return rc;
}

static int auth_headers(kis_t *k, const char *tr_id, char bufs[5][1100], const char **h) {
    char tok[1024];
    read_token(k, tok, sizeof tok);
    if (!tok[0]) return -1;
    snprintf(bufs[0], 1100, "Content-Type: application/json; charset=utf-8");
    snprintf(bufs[1], 1100, "authorization: Bearer %s", tok);
    snprintf(bufs[2], 1100, "appkey: %s", k->key);
    snprintf(bufs[3], 1100, "appsecret: %s", k->secret);
    snprintf(bufs[4], 1100, "tr_id: %s", tr_id);
    for (int i = 0; i < 5; i++) h[i] = bufs[i];
    return 0;
}

int kis_get_price(kis_t *k, double *px, char *err, size_t errsz) {
    char bufs[5][1100];
    const char *h[5];
    if (auth_headers(k, "FHMIF10000000", bufs, h)) {
        snprintf(err, errsz, "no token");
        return KIS_ERR;
    }
    char url[512];
    snprintf(url, sizeof url,
             "%s/uapi/domestic-futureoption/v1/quotations/inquire-price"
             "?FID_COND_MRKT_DIV_CODE=F&FID_INPUT_ISCD=%s",
             k->base, k->inst);
    http_resp_t r;
    http_request("GET", url, h, 5, NULL, &r);
    int rc = classify(&r, "futs_prpr", err, errsz);
    if (rc == KIS_OK) {
        char s[32];
        json_str(r.body, "futs_prpr", s, sizeof s);
        *px = atof(s);
        if (*px <= 0) {
            snprintf(err, errsz, "futs_prpr=%s (종목코드 %s 확인 필요)", s, k->inst);
            rc = KIS_ERR;
        }
    }
    http_resp_free(&r);
    return rc;
}

int kis_place(kis_t *k, const order_req_t *o, char *odno, char *msg) {
    char bufs[5][1100];
    const char *h[5];
    if (auth_headers(k, "VTTO1101U", bufs, h)) {
        snprintf(msg, IG_MSG_LEN, "no token");
        return KIS_ERR;
    }
    char price[32];
    if (o->market) snprintf(price, sizeof price, "0");
    else snprintf(price, sizeof price, "%.2f", o->price);
    char body[1024];
    snprintf(body, sizeof body,
             "{\"ORD_PRCS_DVSN_CD\":\"02\",\"CANO\":\"%s\",\"ACNT_PRDT_CD\":\"%s\","
             "\"SLL_BUY_DVSN_CD\":\"%s\",\"SHTN_PDNO\":\"%s\",\"ORD_QTY\":\"%d\","
             "\"UNIT_PRICE\":\"%s\",\"NMPR_TYPE_CD\":\"\",\"KRX_NMPR_CNDT_CD\":\"\","
             "\"CTAC_TLNO\":\"\",\"FUOP_ITEM_DVSN_CD\":\"\",\"ORD_DVSN_CD\":\"%s\"}",
             k->cano, k->prdt, o->side == SIDE_BUY ? "02" : "01", k->inst, o->qty, price,
             o->market ? "02" : "01");
    char url[256];
    snprintf(url, sizeof url, "%s/uapi/domestic-futureoption/v1/trading/order", k->base);
    http_resp_t r;
    http_request("POST", url, h, 5, body, &r);
    int rc = classify(&r, "ODNO", msg, IG_MSG_LEN);
    if (rc == KIS_OK) json_str(r.body, "ODNO", odno, IG_ODNO_LEN);
    http_resp_free(&r);
    return rc;
}

int kis_balance(kis_t *k, balance_t *b, char *err, size_t errsz) {
    memset(b, 0, sizeof *b);
    char bufs[5][1100];
    const char *h[5];
    if (auth_headers(k, "VTFO6118R", bufs, h)) {
        snprintf(err, errsz, "no token");
        return KIS_ERR;
    }
    char url[512];
    snprintf(url, sizeof url,
             "%s/uapi/domestic-futureoption/v1/trading/inquire-balance"
             "?CANO=%s&ACNT_PRDT_CD=%s&MGNA_DVSN=01&EXCC_STAT_CD=1&CTX_AREA_FK200=&CTX_AREA_NK200=",
             k->base, k->cano, k->prdt);
    http_resp_t r;
    http_request("GET", url, h, 5, NULL, &r);
    int rc = classify(&r, "tot_dncl_amt", err, errsz);
    if (rc == KIS_OK) {
        char s[48];
        if (json_str(r.body, "tot_dncl_amt", s, sizeof s)) b->cash = atof(s);
        if (json_str(r.body, "evlu_pfls_amt_smtl", s, sizeof s)) b->pnl = atof(s);
        b->ok = 1;
        b->net_known = 0; /* 종목별 잔고(output1 배열) 파싱은 미구현 → 순포지션 대조는 mock에서만 */
    }
    http_resp_free(&r);
    return rc;
}
