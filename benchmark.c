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
//  - outer_scale は「この関数を何回呼ぶか」で制御する（関数内には入れない）
static double run_kernel(double *A, double *B,
                         size_t A_elems,
                         size_t B_elems,      // stride=1 のときの B の要素数
                         size_t elems_per_iter,
                         size_t stride_elems) // dense:1, stride:8 など
{
    // B を chunk に分割したときの外側ループ回数
    size_t outer_iters = B_elems / elems_per_iter;

    double sum = 0.0;

    for (size_t outer = 0; outer < outer_iters; outer++) {
        // A 全体をなめて L1 を吹き飛ばす
        for (size_t i = 0; i < A_elems; i++) {
            sum += A[i];
        }

        // この chunk に対応する B 側の先頭インデックス
        // stride_elems を掛けたぶんだけ B 上の範囲が広がる
        size_t base = outer * elems_per_iter * stride_elems;

        // elems_per_iter 回だけ必ずループする
        // dense:  stride=1  → B[base + 0,1,2,...]
        // stride: stride=8 → B[base + 0,8,16,...]
        for (size_t j = 0; j < elems_per_iter; j++) {
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
            "  outer_scale : repeat run_kernel this many times (default=1)\n",
            argv[0]);
        return 1;
    }

    size_t A_bytes     = strtoull(argv[1], NULL, 0);
    size_t B_bytes     = strtoull(argv[2], NULL, 0);
    size_t chunk_bytes = strtoull(argv[3], NULL, 0);

    int    access_mode = 0;  // 0=dense, 1=strided
    size_t user_stride = 8;  // B の確保にも使うストライド
    size_t outer_scale = 1;  // run_kernel を何回呼ぶか

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

    size_t A_elems     = A_bytes     / sizeof(double);
    size_t B_elems     = B_bytes     / sizeof(double);
    size_t elems_per_iter = chunk_bytes / sizeof(double);

    if (A_elems == 0 || B_elems == 0 || elems_per_iter == 0) {
        fprintf(stderr, "A_bytes, B_bytes, chunk_bytes must be >= sizeof(double)\n");
        return 1;
    }
    if (B_elems % elems_per_iter != 0) {
        fprintf(stderr, "B_bytes must be a multiple of chunk_bytes.\n");
        return 1;
    }

    // user_stride に応じて B の実メモリサイズを広げる
    //
    // run_kernel 1 回で必要な B 要素数は
    //   required = outer_iters * elems_per_iter * stride_elems
    //            = (B_elems / elems_per_iter) * elems_per_iter * stride_elems
    //            = B_elems * stride_elems
    //
    // access_mode == 0 (dense) のとき:
    //   stride_elems = 1
    //   required = B_elems
    //   B_elems_alloc = B_elems * user_stride >= required
    //
    // access_mode == 1 (strided) のとき:
    //   stride_elems = user_stride
    //   required = B_elems * user_stride
    //   B_elems_alloc と一致
    //
    // outer_scale は「何回 run_kernel を繰り返すか」だけに影響し、
    // 追加の領域は不要（同じ領域を何度も読むだけ）。
    size_t B_elems_alloc = B_elems * user_stride;

    // 情報表示
    size_t base_outer_iters = B_elems / elems_per_iter;
    size_t total_outer_iters = base_outer_iters * outer_scale;

    printf("# Params:\n");
    printf("#   A_bytes        = %zu\n", A_bytes);
    printf("#   B_bytes        = %zu\n", B_bytes);
    printf("#   chunk_bytes    = %zu\n", chunk_bytes);
    printf("#   A_elems        = %zu\n", A_elems);
    printf("#   B_elems        = %zu\n", B_elems);
    printf("#   B_elems_alloc  = %zu  (allocated)\n", B_elems_alloc);
    printf("#   elems_per_iter = %zu\n", elems_per_iter);
    printf("#   access_mode    = %d (0=dense,1=strided)\n", access_mode);
    printf("#   user_stride    = %zu (for allocation)\n", user_stride);
    printf("#   stride_elems   = %zu (effective in kernel)\n", stride_elems);
    printf("#   base_outer_iters = %zu (per run_kernel)\n", base_outer_iters);
    printf("#   outer_scale    = %zu (run_kernel repeats)\n", outer_scale);
    printf("#   total_outer_iters = %zu (base_outer_iters * outer_scale)\n",
           total_outer_iters);

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

    double sum = 0.0;

    // outer_scale 回だけ同じカーネルを繰り返す
    // （命令列は同じで、統計量を稼ぐために実行時間とカウントだけ増やす）
    for (size_t rep = 0; rep < outer_scale; rep++) {
        sum += run_kernel(A, B,
                          A_elems,
                          B_elems,
                          elems_per_iter,
                          stride_elems);
    }

    sink = sum; // 最適化防止

    printf("sum = %.6f\n", sum);

    free(A);
    free(B);
    return 0;
}
