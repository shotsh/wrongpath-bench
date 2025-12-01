// wp_bench_minimal.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef INNER_ITERS
#define INNER_ITERS   (1 << 15)
#endif

#ifndef OUTER_ITERS
#define OUTER_ITERS   (1 << 11)
#endif

#ifndef ARRAY_A_SIZE
#define ARRAY_A_SIZE  (1 << 20)   // 1M elements
#endif

#ifndef ARRAY_B_SIZE
#define ARRAY_B_SIZE  (1 << 20)   // 1M elements
#endif

volatile double sink = 0.0;

static void init_array(double *a, size_t n, double base)
{
    for (size_t i = 0; i < n; i++) {
        a[i] = base + (double)i * 0.000001;
    }
}

int main(void)
{
    double *A = (double *)malloc(sizeof(double) * (size_t)ARRAY_A_SIZE);
    double *B = (double *)malloc(sizeof(double) * (size_t)ARRAY_B_SIZE);
    if (!A || !B) {
        fprintf(stderr, "malloc failed\n");
        free(A);
        free(B);
        return 1;
    }

    init_array(A, ARRAY_A_SIZE, 1.0);
    init_array(B, ARRAY_B_SIZE, 1000.0);

    double sum = 0.0;

    for (int outer = 0; outer < OUTER_ITERS; outer++) {
        // 大きな配列 A を何度もなめる inner ループ
        for (int inner = 0; inner < INNER_ITERS; inner++) {
            size_t idx = (size_t)inner % (size_t)ARRAY_A_SIZE;
            sum += A[idx];
        }

        // ループ後の「エピローグ」で B にアクセス
        size_t base = ((size_t)outer * 1024u) % (size_t)ARRAY_B_SIZE;
        for (int k = 0; k < 1024; k++) {
            size_t idx = (base + (size_t)k) % (size_t)ARRAY_B_SIZE;
            sum += B[idx];
        }
    }

    sink = sum;  // 最適化で消されないようにする
    printf("sum = %f\n", sum);

    free(A);
    free(B);
    return 0;
}
