#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define CACHELINE_BYTES 64

volatile double sink = 0.0;

static double run_kernel(double *A, double *B,
                         size_t A_elems,
                         size_t B_elems,
                         size_t chunk_elems,
                         size_t stride_elems)
{
    size_t outer_iters = B_elems / chunk_elems;
    double sum = 0.0;

    for (size_t outer = 0; outer < outer_iters; outer++) {
        for (size_t i = 0; i < A_elems; i++) {
            sum += A[i];
        }

        size_t base = outer * chunk_elems;

        // ここは stride_elems だけでパターンを変える
        for (size_t j = 0; j < chunk_elems; j += stride_elems) {
            sum += B[base + j];
        }
    }

    return sum;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s A_bytes B_bytes chunk_bytes [access_mode] [stride_elems]\n",
                argv[0]);
        fprintf(stderr,
                "  access_mode: 0=dense(stride=1), 1=strided(default: 1 cacheline)\n");
        return 1;
    }

    size_t A_bytes    = strtoull(argv[1], NULL, 0);
    size_t B_bytes    = strtoull(argv[2], NULL, 0);
    size_t chunk_bytes= strtoull(argv[3], NULL, 0);

    int access_mode = 0;
    if (argc >= 5) {
        access_mode = atoi(argv[4]);
    }

    size_t stride_elems;
    if (access_mode == 0) {
        // dense
        stride_elems = 1;
    } else {
        // strided: デフォルトは 1 ライン分
        if (argc >= 6) {
            stride_elems = strtoull(argv[5], NULL, 0);
        } else {
            stride_elems = CACHELINE_BYTES / sizeof(double);
        }
        if (stride_elems == 0) {
            stride_elems = 1;
        }
    }

    size_t A_elems     = A_bytes    / sizeof(double);
    size_t B_elems     = B_bytes    / sizeof(double);
    size_t chunk_elems = chunk_bytes/ sizeof(double);

    if (A_elems == 0 || B_elems == 0 || chunk_elems == 0) {
        fprintf(stderr, "A/B/chunk size too small\n");
        return 1;
    }
    if (B_elems % chunk_elems != 0) {
        fprintf(stderr, "B_elems must be a multiple of chunk_elems\n");
        return 1;
    }

    double *A = aligned_alloc(64, A_elems * sizeof(double));
    double *B = aligned_alloc(64, B_elems * sizeof(double));
    if (!A || !B) {
        perror("alloc");
        return 1;
    }

    for (size_t i = 0; i < A_elems; i++) A[i] = 1.0;
    for (size_t i = 0; i < B_elems; i++) B[i] = 2.0;

    double sum = run_kernel(A, B, A_elems, B_elems, chunk_elems, stride_elems);
    sink = sum; // 最適化殺し

    printf("# Params:\n");
    printf("#   A_bytes     = %zu\n", A_bytes);
    printf("#   B_bytes     = %zu\n", B_bytes);
    printf("#   chunk_bytes = %zu\n", chunk_bytes);
    printf("#   A_elems     = %zu\n", A_elems);
    printf("#   B_elems     = %zu\n", B_elems);
    printf("#   chunk_elems = %zu\n", chunk_elems);
    printf("#   access_mode = %d\n", access_mode);
    printf("#   stride_elems= %zu\n", stride_elems);
    printf("sum = %.6f\n", sum);

    free(A);
    free(B);
    return 0;
}
