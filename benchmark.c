// benchmark.c
#include <stdint.h>
#include <stdlib.h>

#ifndef TRACE_MODE
#include <stdio.h>
#include <time.h>
#endif

#ifndef CACHELINE_BYTES
#define CACHELINE_BYTES 64
#endif

// 最適化で消されないようにする
volatile double sink = 0.0;

static void init_array(double *a, size_t n, double base)
{
    for (size_t i = 0; i < n; i++) {
        a[i] = base + (double)i * 0.000001;
    }
}

// access_mode = 0: 密アクセス（全要素）
// access_mode = 1: 1 ラインにつき 1 要素だけ読むストライドアクセス
static double run_kernel(double *A, double *B,
                         size_t A_elems,
                         size_t B_elems,
                         size_t chunk_elems,
                         int access_mode)
{
    size_t outer_iters = B_elems / chunk_elems;
    size_t line_elems  = CACHELINE_BYTES / sizeof(double);
    if (line_elems == 0) {
        line_elems = 1;
    }

    double sum = 0.0;

    for (size_t outer = 0; outer < outer_iters; outer++) {
        // A 全体をなめて L1 を荒らす
        for (size_t i = 0; i < A_elems; i++) {
            sum += A[i];
        }

        // B は outer ごとに chunk_elems だけ読む
        size_t base = outer * chunk_elems;

        if (access_mode == 0) {
            // 密アクセス
            for (size_t j = 0; j < chunk_elems; j++) {
                sum += B[base + j];
            }
        } else {
            // 1 ラインにつき 1 要素だけ読む
            for (size_t j = 0; j < chunk_elems; j += line_elems) {
                sum += B[base + j];
            }
        }
    }

    return sum;
}

int main(int argc, char **argv)
{
    // デフォルト値
    size_t A_bytes     = 64 * 1024;        // 64 KiB
    size_t B_bytes     = 512 * 1024 * 1024; // 512 MiB
    size_t chunk_bytes = 512 * 1024;       // 512 KiB
    int access_mode    = 0;                // 0: 密, 1: line-stride

    if (argc >= 2) A_bytes     = strtoull(argv[1], NULL, 0);
    if (argc >= 3) B_bytes     = strtoull(argv[2], NULL, 0);
    if (argc >= 4) chunk_bytes = strtoull(argv[3], NULL, 0);
    if (argc >= 5) access_mode = atoi(argv[4]);

    size_t A_elems     = A_bytes     / sizeof(double);
    size_t B_elems     = B_bytes     / sizeof(double);
    size_t chunk_elems = chunk_bytes / sizeof(double);

    if (A_elems == 0 || B_elems == 0 || chunk_elems == 0 || B_elems < chunk_elems) {
#ifndef TRACE_MODE
        fprintf(stderr, "bad params: A_bytes=%zu B_bytes=%zu chunk_bytes=%zu\n",
                A_bytes, B_bytes, chunk_bytes);
#endif
        return 1;
    }

    // B は chunk の整数倍だけ使う
    B_elems = (B_elems / chunk_elems) * chunk_elems;
    B_bytes = B_elems * sizeof(double);

#ifndef TRACE_MODE
    printf("# Params:\n");
    printf("#   A_bytes     = %zu\n", A_bytes);
    printf("#   B_bytes     = %zu\n", B_bytes);
    printf("#   chunk_bytes = %zu\n", chunk_bytes);
    printf("#   A_elems     = %zu\n", A_elems);
    printf("#   B_elems     = %zu\n", B_elems);
    printf("#   chunk_elems = %zu\n", chunk_elems);
    printf("#   access_mode = %d (0=dense, 1=line_stride)\n", access_mode);
#endif

    double *A = (double *)malloc(A_elems * sizeof(double));
    double *B = (double *)malloc(B_elems * sizeof(double));
    if (!A || !B) {
#ifndef TRACE_MODE
        fprintf(stderr, "malloc failed\n");
#endif
        free(A);
        free(B);
        return 1;
    }

    init_array(A, A_elems, 1.0);
    init_array(B, B_elems, 1000.0);

#ifndef TRACE_MODE
    clock_t t0 = clock();
#endif

    double sum = run_kernel(A, B, A_elems, B_elems, chunk_elems, access_mode);
    sink = sum;

#ifndef TRACE_MODE
    clock_t t1 = clock();
    double elapsed_sec = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;

    printf("sum = %.6f\n", sum);
    printf("elapsed_sec = %.6f\n", elapsed_sec);
#endif

    free(A);
    free(B);
    return 0;
}
