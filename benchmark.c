// benchmark.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static volatile double sink = 0.0;

static void init_array(double *p, size_t n, double base)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = base + (double)i * 0.000001;
    }
}

// A で L1 を荒らしつつ、C を chunk ごとにアクセスするカーネル
//  - outer/inner のループ回数は stride に依存しない
//  - stride_elems だけが dense/stride の違いを表現する
static double run_kernel(double *A, double *C,
                         size_t A_elems,
                         size_t logical_B_elems,  // stride=1 のときに相当する「論理 B サイズ」
                         size_t chunk_elems,
                         size_t stride_elems)     // dense:1, stride:8 など
{
    // 「論理 B」を chunk に分割したときの外側ループ回数
    size_t outer_iters = logical_B_elems / chunk_elems;

    double sum = 0.0;

    for (size_t outer = 0; outer < outer_iters; outer++) {
        // A 全体をなめて L1 を毎回吹き飛ばす役
        for (size_t i = 0; i < A_elems; i++) {
            sum += A[i];
        }

        // この chunk に対応する C 側の先頭インデックス
        // stride_elems を掛けたぶんだけ C 上の範囲が広がる
        size_t base = outer * chunk_elems * stride_elems;

        // chunk_elems 回だけ必ずループする
        // dense:  stride=1 → C[base + 0,1,2,...]
        // stride: stride=8 → C[base + 0,8,16,...]
        for (size_t j = 0; j < chunk_elems; j++) {
            size_t idx = base + j * stride_elems;
            sum += C[idx];
        }
    }

    return sum;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s A_bytes B_bytes chunk_bytes [access_mode] [stride_elems]\n"
            "  access_mode: 0=dense, 1=strided (default=0)\n"
            "  stride_elems: used only when access_mode=1 (default=8)\n",
            argv[0]);
        return 1;
    }

    size_t A_bytes     = strtoull(argv[1], NULL, 0);
    size_t B_bytes     = strtoull(argv[2], NULL, 0);  // 「論理 B サイズ」
    size_t chunk_bytes = strtoull(argv[3], NULL, 0);

    int access_mode = 0;
    size_t user_stride = 8;   // デフォルトのストライド

    if (argc >= 5) {
        access_mode = atoi(argv[4]);  // 0 or 1
    }
    if (argc >= 6) {
        user_stride = strtoull(argv[5], NULL, 0);
        if (user_stride == 0) {
            fprintf(stderr, "stride_elems must be >= 1\n");
            return 1;
        }
    }

    // dense のときは stride=1 として扱う（ここで分岐しておけば、カーネル内には分岐が出ない）
    size_t stride_elems = (access_mode == 0) ? 1 : user_stride;

    size_t A_elems          = A_bytes     / sizeof(double);
    size_t logical_B_elems  = B_bytes     / sizeof(double);
    size_t chunk_elems      = chunk_bytes / sizeof(double);

    if (A_elems == 0 || logical_B_elems == 0 || chunk_elems == 0) {
        fprintf(stderr, "A_bytes, B_bytes, chunk_bytes must be >= sizeof(double)\n");
        return 1;
    }
    if (logical_B_elems % chunk_elems != 0) {
        fprintf(stderr, "B_bytes must be a multiple of chunk_bytes.\n");
        return 1;
    }

    // stride_elems に応じて C の実メモリサイズを広げる
    //  dense:  stride=1 → C_elems = logical_B_elems (B 相当)
    //  stride: stride=8 → C_elems = logical_B_elems * 8 （より大きいワーキングセット）
    size_t C_elems = logical_B_elems * stride_elems;

    printf("# Params:\n");
    printf("#   A_bytes     = %zu\n", A_bytes);
    printf("#   B_bytes     = %zu  (logical)\n", B_bytes);
    printf("#   chunk_bytes = %zu\n", chunk_bytes);
    printf("#   A_elems     = %zu\n", A_elems);
    printf("#   B_elems     = %zu  (logical)\n", logical_B_elems);
    printf("#   C_elems     = %zu  (allocated)\n", C_elems);
    printf("#   chunk_elems = %zu\n", chunk_elems);
    printf("#   access_mode = %d (0=dense,1=strided)\n", access_mode);
    printf("#   stride_elems= %zu (effective)\n", stride_elems);

    double *A = (double *)malloc(sizeof(double) * A_elems);
    double *C = (double *)malloc(sizeof(double) * C_elems);
    if (!A || !C) {
        fprintf(stderr, "malloc failed\n");
        free(A);
        free(C);
        return 1;
    }

    init_array(A, A_elems, 1.0);
    init_array(C, C_elems, 1000.0);

    double sum = run_kernel(A, C,
                            A_elems,
                            logical_B_elems,
                            chunk_elems,
                            stride_elems);

    sink = sum; // 最適化防止

    printf("sum = %.6f\n", sum);

    free(A);
    free(C);
    return 0;
}
