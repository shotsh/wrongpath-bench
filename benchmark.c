// benchmark.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef TRACE_MODE
#warning "Compiling with TRACE_MODE: B array is NOT initialized (trace only build)"
#endif

static volatile double sink = 0.0;

static void init_array(double *p, size_t n, double base)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = base + (double)i * 0.000001;
    }
}

// Kernel that thrashes L1 using A while accessing B in chunks
//  - The outer/inner loop counts do not depend on stride
//  - Only stride_elems represents the difference between dense/strided
//  - outer_scale is controlled by "how many times we call this function" (kept outside the function)
static double run_kernel(double *A, double *B,
                         size_t A_elems,
                         size_t B_elems,      // B element count when stride=1
                         size_t elems_per_iter,
                         size_t stride_elems) // dense:1, stride:8, etc.
{
    // Number of outer-loop iterations when B is divided into chunks
    size_t outer_iters = B_elems / elems_per_iter;

    double sum = 0.0;

    for (size_t outer = 0; outer < outer_iters; outer++) {
        // Sweep the entire A to blow L1 away
        for (size_t i = 0; i < A_elems; i++) {
            sum += A[i];
        }

        // Starting index in B corresponding to this chunk
        // The range on B expands by stride_elems
        size_t base = outer * elems_per_iter * stride_elems;

        // Always loop exactly elems_per_iter times
        // dense:  stride=1  -> B[base + 0,1,2,...]
        // stride: stride=8 -> B[base + 0,8,16,...]
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
    size_t user_stride = 8;  // Stride also used to size the B allocation
    size_t outer_scale = 1;  // How many times to call run_kernel

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

    // Treat dense as stride=1 (no branches inside the kernel)
    size_t stride_elems = (access_mode == 0) ? 1 : user_stride;

    size_t A_elems        = A_bytes       / sizeof(double);
    size_t B_elems        = B_bytes       / sizeof(double);
    size_t elems_per_iter = chunk_bytes   / sizeof(double);

    if (A_elems == 0 || B_elems == 0 || elems_per_iter == 0) {
        fprintf(stderr, "A_bytes, B_bytes, chunk_bytes must be >= sizeof(double)\n");
        return 1;
    }
    if (B_elems % elems_per_iter != 0) {
        fprintf(stderr, "B_bytes must be a multiple of chunk_bytes.\n");
        return 1;
    }

    // Enlarge the actual memory size of B according to user_stride
    //
    // The number of B elements needed for a single run of run_kernel is:
    //   required = outer_iters * elems_per_iter * stride_elems
    //            = (B_elems / elems_per_iter) * elems_per_iter * stride_elems
    //            = B_elems * stride_elems
    //
    // When access_mode == 0 (dense):
    //   stride_elems = 1
    //   required = B_elems
    //   B_elems_alloc = B_elems * user_stride >= required
    //
    // When access_mode == 1 (strided):
    //   stride_elems = user_stride
    //   required = B_elems * user_stride
    //   Matches B_elems_alloc
    //
    // outer_scale only affects "how many times run_kernel is repeated",
    // and no additional space is needed (the same region is read multiple times).
    size_t B_elems_alloc = B_elems * user_stride;

    // Print configuration info
    size_t base_outer_iters  = B_elems / elems_per_iter;
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

#ifdef TRACE_MODE
    printf("#   TRACE_MODE: B is not initialized (values arbitrary, address pattern only)\n");
#endif

    double *A = (double *)malloc(sizeof(double) * A_elems);
    double *B = (double *)malloc(sizeof(double) * B_elems_alloc);
    if (!A || !B) {
        fprintf(stderr, "malloc failed\n");
        free(A);
        free(B);
        return 1;
    }

#ifndef TRACE_MODE
    // Normal build: initialize both A and B for correct numeric behavior and perf experiments
    init_array(A, A_elems, 1.0);
    init_array(B, B_elems_alloc, 1000.0);
#else
    // TRACE_MODE build: initialize only A. B is left uninitialized to avoid large init overhead.
    init_array(A, A_elems, 1.0);
#endif

    double sum = 0.0;

    // Repeat the same kernel outer_scale times
    // (The instruction stream is the same; we just increase runtime and counts to gather statistics)
    for (size_t rep = 0; rep < outer_scale; rep++) {
        sum += run_kernel(A, B,
                          A_elems,
                          B_elems,
                          elems_per_iter,
                          stride_elems);
    }

    sink = sum; // Prevent optimization

    printf("sum = %.6f\n", sum);

    free(A);
    free(B);
    return 0;
}
