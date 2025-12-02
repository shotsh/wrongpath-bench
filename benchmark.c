#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* =========================
 * 設定用マクロ
 * ========================= */

// キャッシュラインサイズ（必要に応じて -DCACHELINE_BYTES=64 などで上書き）
#ifndef CACHELINE_BYTES
#define CACHELINE_BYTES 64
#endif

// 0 = 密アクセス (dense), 1 = ストライドアクセス
#ifndef ACCESS_MODE
#define ACCESS_MODE 0
#endif

// ストライド長（double 要素数単位）
// ACCESS_MODE == 1 のときだけ使用。
// デフォルトは「1 ラインにつき 1 要素」相当。
#ifndef STRIDE_ELEMS
#define STRIDE_ELEMS (CACHELINE_BYTES / sizeof(double))
#endif

// 最適化で計算が消されないようにするためのグローバル
volatile double sink = 0.0;

/* =========================
 * ヘルパ
 * ========================= */

static void
init_array(double *a, size_t n, double base)
{
    for (size_t i = 0; i < n; i++) {
        a[i] = base + (double)(i & 0xFF) * 0.001;
    }
}

/*
 * カーネル本体
 *
 * A: 小さい配列（毎回全部なめて L1 を荒らす）
 * B: 大きい配列（chunk ごとに読む。ここが実験対象）
 *
 * ACCESS_MODE, STRIDE_ELEMS はコンパイル時マクロで切り替え。
 * 実行時の if 分岐は一切入らないようにしている。
 */
static double
run_kernel(double *A, double *B,
           size_t A_elems,
           size_t B_elems,
           size_t chunk_elems)
{
    const size_t outer_iters = B_elems / chunk_elems;
    double sum = 0.0;

    for (size_t outer = 0; outer < outer_iters; outer++) {
        /* A 全体をなめて L1 を汚す */
        for (size_t i = 0; i < A_elems; i++) {
            sum += A[i];
        }

        const size_t base = outer * chunk_elems;

        /* ここから下はコンパイル時に分岐が落ちる */
#if ACCESS_MODE == 0
        /* 密アクセス: chunk 内の全要素を読む */
        for (size_t j = 0; j < chunk_elems; j++) {
            sum += B[base + j];
        }
#elif ACCESS_MODE == 1
        /* ストライドアクセス:
         * STRIDE_ELEMS ごとに 1 要素だけ読む
         * （例: STRIDE_ELEMS=8 なら 1 ラインに 1 要素）
         */
        for (size_t j = 0; j < chunk_elems; j += STRIDE_ELEMS) {
            sum += B[base + j];
        }
#else
#error "ACCESS_MODE must be 0 (dense) or 1 (strided)"
#endif
    }

    return sum;
}

/* =========================
 * main
 * =========================
 *
 * 引数:
 *   argv[1] = A_bytes     （小さい配列 A のバイト数）
 *   argv[2] = B_bytes     （大きい配列 B のバイト数）
 *   argv[3] = chunk_bytes （B を読むときの 1 チャンクのバイト数）
 *
 * 省略時のデフォルト:
 *   A_bytes     = 32 KiB
 *   B_bytes     = 512 MiB
 *   chunk_bytes = 512 KiB
 */

int
main(int argc, char **argv)
{
    size_t A_bytes     = 32 * 1024;          /* 32 KiB */
    size_t B_bytes     = 512ull * 1024 * 1024; /* 512 MiB */
    size_t chunk_bytes = 512 * 1024;        /* 512 KiB */

    if (argc >= 2) {
        A_bytes = strtoull(argv[1], NULL, 0);
    }
    if (argc >= 3) {
        B_bytes = strtoull(argv[2], NULL, 0);
    }
    if (argc >= 4) {
        chunk_bytes = strtoull(argv[3], NULL, 0);
    }

    if (A_bytes == 0 || B_bytes == 0 || chunk_bytes == 0) {
        fprintf(stderr, "A_bytes, B_bytes, chunk_bytes must be > 0\n");
        return 1;
    }
    if (B_bytes % chunk_bytes != 0) {
        fprintf(stderr, "B_bytes must be a multiple of chunk_bytes\n");
        return 1;
    }
    if (A_bytes % sizeof(double) != 0 ||
        B_bytes % sizeof(double) != 0 ||
        chunk_bytes % sizeof(double) != 0) {
        fprintf(stderr, "All sizes must be multiples of sizeof(double)\n");
        return 1;
    }

    const size_t A_elems     = A_bytes / sizeof(double);
    const size_t B_elems     = B_bytes / sizeof(double);
    const size_t chunk_elems = chunk_bytes / sizeof(double);

    printf("# Params:\n");
    printf("#   A_bytes     = %zu\n", A_bytes);
    printf("#   B_bytes     = %zu\n", B_bytes);
    printf("#   chunk_bytes = %zu\n", chunk_bytes);
    printf("#   A_elems     = %zu\n", A_elems);
    printf("#   B_elems     = %zu\n", B_elems);
    printf("#   chunk_elems = %zu\n", chunk_elems);
    printf("#   ACCESS_MODE = %d (0=dense, 1=strided)\n", ACCESS_MODE);
    printf("#   STRIDE_ELEMS= %d\n", STRIDE_ELEMS);

    double *A = (double *)malloc(A_elems * sizeof(double));
    double *B = (double *)malloc(B_elems * sizeof(double));
    if (!A || !B) {
        fprintf(stderr, "malloc failed\n");
        free(A);
        free(B);
        return 1;
    }

    init_array(A, A_elems, 1.0);
    init_array(B, B_elems, 1000.0);

    double sum = run_kernel(A, B, A_elems, B_elems, chunk_elems);

    sink = sum;  /* 最適化防止 */

    printf("sum = %.6f\n", sum);

    free(A);
    free(B);
    return 0;
}
