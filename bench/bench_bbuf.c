/* 벤치마크
 *  1) 유한 버퍼 처리량: 생산자/소비자 수에 따른 ops/s (세마포 연산 비용 + 경합 비용)
 *  2) 게이트웨이 워커 풀 확장성: 브로커 호출 1건 = 네트워크 20ms(I/O 대기)일 때
 *     워커 수 M에 따른 주문 처리량. I/O 대기 중에는 CPU를 쓰지 않으므로, M개 스레드가 대기를 겹치게 해서
 *     처리량이 M배에 가깝게 늘어야 한다. 이 시스템에서 멀티스레드가 이득을 주는 지점이 바로 여기다.
 *     (반대로 전략 계산은 μs 단위라 스레드를 늘려도 이득이 거의 없다 → 전략 스레드는 "격리" 목적)
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bbuf.h"
#include "common.h"

static bbuf_t B;
static long per_prod;

static void *prod(void *x) {
    (void)x;
    for (long i = 0; i < per_prod; i++) {
        uint64_t v = (uint64_t)i;
        bbuf_put(&B, &v);
    }
    return NULL;
}

static void *cons(void *x) {
    long *n = x;
    for (;;) {
        uint64_t v;
        bbuf_get(&B, &v);
        if (v == UINT64_MAX) return NULL;
        (*n)++;
    }
}

static double bench_bbuf(int P, int C, long total) {
    bbuf_init(&B, 64, sizeof(uint64_t));
    per_prod = total / P;
    pthread_t pt[32], ct[32];
    long cnt[32] = {0};
    int64_t t0 = ig_now_ns();
    for (int i = 0; i < C; i++) pthread_create(&ct[i], NULL, cons, &cnt[i]);
    for (int i = 0; i < P; i++) pthread_create(&pt[i], NULL, prod, NULL);
    for (int i = 0; i < P; i++) pthread_join(pt[i], NULL);
    uint64_t poison = UINT64_MAX;
    for (int i = 0; i < C; i++) bbuf_put(&B, &poison);
    for (int i = 0; i < C; i++) pthread_join(ct[i], NULL);
    double s = (ig_now_ns() - t0) / 1e9;
    bbuf_destroy(&B);
    return (double)(per_prod * P) / s;
}

/* ---- 워커 풀 ---- */
static bbuf_t Q;
static atomic_long done;

static void *gw(void *x) {
    (void)x;
    for (;;) {
        int v;
        bbuf_get(&Q, &v);
        if (v < 0) return NULL;
        ig_sleep_ns(20000000); /* 브로커 왕복 20ms */
        atomic_fetch_add(&done, 1);
    }
}

static double bench_pool(int M, int orders) {
    bbuf_init(&Q, 64, sizeof(int));
    atomic_store(&done, 0);
    pthread_t t[32];
    int64_t t0 = ig_now_ns();
    for (int i = 0; i < M; i++) pthread_create(&t[i], NULL, gw, NULL);
    for (int i = 0; i < orders; i++) bbuf_put(&Q, &i);
    int p = -1;
    for (int i = 0; i < M; i++) bbuf_put(&Q, &p);
    for (int i = 0; i < M; i++) pthread_join(t[i], NULL);
    double s = (ig_now_ns() - t0) / 1e9;
    bbuf_destroy(&Q);
    return orders / s;
}

int main(void) {
    printf("[1] 유한 버퍼 처리량 (cap=64, 40만 건)\n");
    printf("    생산자\\소비자      1          2          4          8\n");
    int ns[] = {1, 2, 4, 8};
    for (int a = 0; a < 4; a++) {
        printf("    %-6d      ", ns[a]);
        for (int b = 0; b < 4; b++) printf("%8.0fk  ", bench_bbuf(ns[a], ns[b], 400000) / 1e3);
        printf("  ops/s\n");
    }
    printf("\n[2] 게이트웨이 워커 풀 확장성 (주문 160건, 브로커 호출 20ms)\n");
    printf("    워커 수   처리량(건/s)   가속비   이상적\n");
    double base = 0;
    int ms[] = {1, 2, 4, 8, 16};
    for (int i = 0; i < 5; i++) {
        double r = bench_pool(ms[i], 160);
        if (i == 0) base = r;
        printf("    %-8d %10.1f   %6.2fx   %5dx\n", ms[i], r, r / base, ms[i]);
    }
    printf("    → 실제 KIS에서는 초당 한도(레이트 리미터)가 상한이므로, 워커 수는 \"한도 × 왕복시간\" 정도면 충분\n"
           "      (예: 한도 10건/s × 왕복 0.03s ≈ 0.3개 → 평균엔 1개면 되지만, 여러 전략이 동시에 진입하는 burst 대비 기본 4개)\n");
    return 0;
}
