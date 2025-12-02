#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

/*
 * Usage:
 *   ./benchmark A_bytes B_bytes chunk_bytes outer_iters
 *
 * 例:
 *   ./benchmark $((32*1024)) $((512*1024*1024)) $((512*1024)) 0
 *
 * 単位はすべて「バイト」。
 * outer_iters=0 の場合は outer_iters = B_bytes / chunk_bytes に自動設定。
 */

/* 単純な経過時間測定（perf だけ使うなら無視してOK） */
static double now_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s A_bytes B_bytes chunk_bytes outer_iters\n"
                "  All sizes are in bytes.\n"
                "  If outer_iters == 0, it will be set to B_bytes / chunk_bytes.\n\n"
                "Example:\n"
                "  %s $((32*1024)) $((512*1024*1024)) $((512*1024)) 0\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    unsigned long long A_bytes_ull     = strtoull(argv[1], NULL, 0);
    unsigned long long B_bytes_ull     = strtoull(argv[2], NULL, 0);
    unsigned long long chunk_bytes_ull = strtoull(argv[3], NULL, 0);
    unsigned long long outer_iters     = strtoull(argv[4], NULL, 0);

    if (A_bytes_ull == 0 || B_bytes_ull == 0 || chunk_bytes_ull == 0) {
        fprintf(stderr, "Error: A_bytes, B_bytes, and chunk_bytes must be > 0.\n");
        return EXIT_FAILURE;
    }

    /* size_t に安全に収まる前提でキャスト（64bit Linux を想定） */
    size_t A_bytes     = (size_t)A_bytes_ull;
    size_t B_bytes     = (size_t)B_bytes_ull;
    size_t chunk_bytes = (size_t)chunk_bytes_ull;

    /* 要素数（double 配列とする） */
    size_t A_elems     = A_bytes     / sizeof(double);
    size_t B_elems     = B_bytes     / sizeof(double);
    size_t chunk_elems = chunk_bytes / sizeof(double);

    if (A_elems == 0 || B_elems == 0 || chunk_elems == 0) {
        fprintf(stderr,
                "Error: sizes are too small; need at least sizeof(double) bytes.\n");
        return EXIT_FAILURE;
    }

    /* outer_iters が 0 の場合は自動設定 */
    unsigned long long max_outer = (unsigned long long)(B_elems / chunk_elems);
    if (max_outer == 0) {
        fprintf(stderr,
                "Error: B_bytes is smaller than one chunk (B_elems < chunk_elems).\n");
        return EXIT_FAILURE;
    }

    if (outer_iters == 0) {
        outer_iters = max_outer;  /* ちょうど B 全体を 1 周するイメージ */
    }

    if (outer_iters > max_outer) {
        fprintf(stderr,
                "Error: require outer_iters * chunk_elems <= B_elems.\n"
                "  B_elems        = %zu\n"
                "  chunk_elems    = %zu\n"
                "  max_outer      = %llu\n"
                "  requested outer_iters = %llu\n",
                B_elems, chunk_elems,
                (unsigned long long)max_outer,
                outer_iters);
        return EXIT_FAILURE;
    }

    fprintf(stderr,
            "# Params:\n"
            "#   A_bytes     = %llu (%.3f KiB)\n"
            "#   B_bytes     = %llu (%.3f MiB)\n"
            "#   chunk_bytes = %llu (%.3f KiB)\n"
            "#   A_elems     = %zu\n"
            "#   B_elems     = %zu\n"
            "#   chunk_elems = %zu\n"
            "#   outer_iters = %llu\n"
            "#   B_covered   = outer_iters * chunk_bytes = %.3f MiB\n\n",
            A_bytes_ull,     A_bytes_ull     / 1024.0,
            B_bytes_ull,     B_bytes_ull     / (1024.0 * 1024.0),
            chunk_bytes_ull, chunk_bytes_ull / 1024.0,
            A_elems, B_elems, chunk_elems,
            outer_iters,
            (outer_iters * (double)chunk_bytes_ull) / (1024.0 * 1024.0));

    /* メモリ確保 */
    double *A = (double *)malloc((size_t)A_elems * sizeof(double));
    double *B = (double *)malloc((size_t)B_elems * sizeof(double));
    if (!A || !B) {
        fprintf(stderr, "Error: malloc failed (A or B).\n");
        free(A);
        free(B);
        return EXIT_FAILURE;
    }

    /* 初期化（値はなんでもよいが、多少ランダムっぽくしておく） */
    for (size_t i = 0; i < A_elems; ++i) {
        A[i] = (double)((i * 1315423911u) & 0xFFu);
    }
    for (size_t i = 0; i < B_elems; ++i) {
        B[i] = (double)(((i * 2654435761u) + 12345u) & 0xFFu);
    }

    /* 必要ならウォームアップで 1 回軽くなめてもよい（コメントアウト中） */
    /*
    for (size_t i = 0; i < A_elems; ++i) {
        volatile double tmp = A[i];
        (void)tmp;
    }
    for (size_t i = 0; i < B_elems; ++i) {
        volatile double tmp = B[i];
        (void)tmp;
    }
    */

    double t0 = now_sec();

    double sum = 0.0;

    /* outer loop: A を毎回全部なめつつ、
       B の中から chunk_elems 要素ずつ順番にアクセス */
    for (unsigned long long outer = 0; outer < outer_iters; ++outer) {
        /* A: 毎回「同じ領域」を sequential にぐるぐる */
        for (size_t i = 0; i < A_elems; ++i) {
            sum += A[i];
        }

        /* B: outer ごとに chunk_elems ずつ前進（wrap しない前提） */
        size_t chunk_start = (size_t)(outer * (unsigned long long)chunk_elems);

        for (size_t j = 0; j < chunk_elems; ++j) {
            sum += B[chunk_start + j];
        }
    }

    double t1 = now_sec();

    /* sum を volatile 経由で書き出して、最適化で消されないようにする */
    volatile double sink = sum;
    printf("sum = %.6f\n", sink);
    printf("elapsed_sec = %.6f\n", t1 - t0);

    free(A);
    free(B);
    return EXIT_SUCCESS;
}
