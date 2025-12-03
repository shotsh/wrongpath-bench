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

// A で L1 を荒らしつつ、B を chunk ごとにアクセスするカーネル
//  - outer/inner のループ回数は stride に依存しない
//  - stride_elems だけが dense/stride の違いを表現する
//  - outer_scale でループ全体を何回分まわすかを増やせる
//    （その分 B は十分大きく確保されている前提）
static double run_kernel(double *A, double *B,
                         size_t A_elems,
                         size_t B_elems_logical, // stride=1 のときの「論理 B サイズ」
                         size_t chunk_elems,
                         size_t stride_elems,    // dense:1, stride:8 など
                         size_t outer_scale)
{
    // 「論理 B」を chunk に分割したときの外側ループ回数
    size_t base_outer_iters = B_elems_logical / chunk_elems;
    size_t outer_iters      = base_outer_iters * outer_scale;

    double sum = 0.0;

    for (size_t outer = 0; outer < outer_iters; outer++) {
        // A 全体をなめて L1 を吹き飛ばす
        for (size_t i = 0; i < A_elems; i++) {
            sum += A[i];
        }

        // この chunk に対応する B 側の先頭インデックス
        // stride_elems を掛けたぶんだけ B 上の範囲が広がる
        size_t base = outer * chunk_elems * stride_elems;

        // chunk_elems 回だけ必ずループする
        // dense:  stride=1  → B[base + 0,1,2,...]
        // stride: stride=8 → B[base + 0,8,16,...]
        for (size_t j = 0; j < chunk_elems; j++) {
            size_t idx = base + j * stride_elems;
            sum += B[idx];
        }
    }

    return sum;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s A_bytes B_bytes chunk_bytes [access_mode] [stride_elems] [outer_scale]\n"
            "  access_mode : 0=dense, 1=strided (default=0)\n"
            "  stride_elems: used only when access_mode=1, but also controls B allocation (default=8)\n"
            "  outer_scale : multiply outer loop count (default=1)\n",
            argv[0]);
        return 1;
    }

    size_t A_bytes     = strtoull(argv[1], NULL, 0);
    size_t B_bytes     = strtoull(argv[2], NULL, 0);  // 「論理 B サイズ」
    size_t chunk_bytes = strtoull(argv[3], NULL, 0);

    int access_mode    = 0;   // 0=dense, 1=strided
    size_t user_stride = 8;   // 配列 B の確保にも使うストライド
    size_t outer_scale = 1;   // ループを何周分まわすか

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
    if (argc >= 7) {
        outer_scale = strtoull(argv[6], NULL, 0);
        if (outer_scale == 0) {
            fprintf(stderr, "outer_scale must be >= 1\n");
            return 1;
        }
    }

    // dense のときは stride=1 として扱う（カーネル内には分岐を出さない）
    size_t stride_elems = (access_mode == 0) ? 1 : user_stride;

    size_t A_elems         = A_bytes     / sizeof(double);
    size_t B_elems_logical = B_bytes     / sizeof(double);
    size_t chunk_elems     = chunk_bytes / sizeof(double);

    if (A_elems == 0 || B_elems_logical == 0 || chunk_elems == 0) {
        fprintf(stderr, "A_bytes, B_bytes, chunk_bytes must be >= sizeof(double)\n");
        return 1;
    }
    if (B_elems_logical % chunk_elems != 0) {
        fprintf(stderr, "B_bytes must be a multiple of chunk_bytes.\n");
        return 1;
    }

    // user_stride と outer_scale に応じて B の実メモリサイズを広げる
    //
    // base_outer_iters = B_elems_logical / chunk_elems
    // outer_iters      = base_outer_iters * outer_scale
    // 必要な B の要素数は
    //   required = outer_iters * chunk_elems * stride_elems
    //
    // access_mode == 0 (dense) のとき:
    //   stride_elems = 1
    //   required = B_elems_logical * outer_scale
    //   B_elems_alloc = B_elems_logical * user_stride * outer_scale
    //                 = required * user_stride (十分大きい)
    //
    // access_mode == 1 (strided) のとき:
    //   stride_elems = user_stride
    //   required = B_elems_logical * user_stride * outer_scale
    //   B_elems_alloc と一致
    size_t B_elems_alloc = B_elems_logical * user_stride * outer_scale;

    printf("# Params:\n");
    printf("#   A_bytes      = %zu\n", A_bytes);
    printf("#   B_bytes      = %zu  (logical)\n", B_bytes);
    printf("#   chunk_bytes  = %zu\n", chunk_bytes);
    printf("#   A_elems      = %zu\n", A_elems);
    printf("#   B_elems      = %zu  (logical)\n", B_elems_logical);
    printf("#   B_elems_alloc= %zu  (allocated)\n", B_elems_alloc);
    printf("#   chunk_elems  = %zu\n", chunk_elems);
    printf("#   access_mode  = %d (0=dense,1=strided)\n", access_mode);
    printf("#   user_stride  = %zu (for allocation)\n", user_stride);
    printf("#   stride_elems = %zu (effective in kernel)\n", stride_elems);
    printf("#   outer_scale  = %zu\n", outer_scale);

    double *A = (double *)malloc(sizeof(double) * A_elems);
    double *B = (double *)malloc(sizeof(double) * B_elems_alloc);
    if (!A || !B) {
        fprintf(stderr, "malloc failed\n");
        free(A);
        free(B);
        return 1;
    }

    init_array(A, A_elems, 1.0);
    init_array(B, B_elems_alloc, 1000.0);

    double sum = run_kernel(A, B,
                            A_elems,
                            B_elems_logical,
                            chunk_elems,
                            stride_elems,
                            outer_scale);

    sink = sum; // 最適化防止

    printf("sum = %.6f\n", sum);

    free(A);
    free(B);
    return 0;
}
