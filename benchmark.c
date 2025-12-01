#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#ifndef INNER_ITERS
#define INNER_ITERS   (1 << 15)   // 32768
#endif

#ifndef OUTER_ITERS
#define OUTER_ITERS   (1 << 11)   // 2048
#endif

#ifndef ARRAY_A_SIZE
#define ARRAY_A_SIZE  (1 << 20)   // 1M elements ≈ 8MB (double)
#endif

#ifndef ARRAY_B_SIZE
#define ARRAY_B_SIZE  (1 << 20)   // 1M elements ≈ 8MB (double)
#endif

// 最適化で消されないようにするためのグローバル
volatile double sink = 0.0;

static void init_array(double *a, size_t n, double base)
{
    for (size_t i = 0; i < n; i++) {
        a[i] = base + (double)i * 0.000001;
    }
}

int main(void)
{
    printf("OUTER_ITERS=%d, INNER_ITERS=%d\n", OUTER_ITERS, INNER_ITERS);
    printf("ARRAY_A_SIZE=%d, ARRAY_B_SIZE=%d\n", ARRAY_A_SIZE, ARRAY_B_SIZE);

    double *A = (double *)malloc(sizeof(double) * ARRAY_A_SIZE);
    double *B = (double *)malloc(sizeof(double) * ARRAY_B_SIZE);
    if (!A || !B) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    init_array(A, ARRAY_A_SIZE, 1.0);
    init_array(B, ARRAY_B_SIZE, 1000.0);

    double sum = 0.0;

    clock_t start = clock();

    for (int outer = 0; outer < OUTER_ITERS; outer++) {
        // 内側ループ: 大きな配列 A を毎回なめる（L1 を荒らす役）
        for (int inner = 0; inner < INNER_ITERS; inner++) {
            size_t idx = (size_t)inner % ARRAY_A_SIZE;
            sum += A[idx];
        }

        // ループ終了後の「エピローグ」に相当する部分で B にアクセスする
        // 本当はここを「少し早めに」実行してプリフェッチしたい、という想定
        size_t base = ((size_t)outer * 1024) % ARRAY_B_SIZE;
        for (int k = 0; k < 1024; k++) {
            size_t idx = (base + (size_t)k) % ARRAY_B_SIZE;
            sum += B[idx];
        }
    }

    clock_t end = clock();

    sink = sum; // 最適化防止

    double elapsed_sec = (double)(end - start) / CLOCKS_PER_SEC;
    printf("Done. sum=%f, time=%.3f sec\n", sum, elapsed_sec);

    free(A);
    free(B);
    return 0;
}
