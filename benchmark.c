// benchmark.c
// Simple two-array microbenchmark:
//   - A: small/medium array to sweep every outer iteration (used to disturb L1)
//   - B: large array, accessed in "chunks" with either dense or strided pattern
//   - The total logical B footprint per run is always B_bytes (independent of stride).

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

/*
 * BENCH_PRINTF:
 *   - If BENCH_VERBOSE is defined, print configuration / sum.
 *   - If not defined, all BENCH_PRINTF calls become no-ops (quiet binary).
 *     This is useful when building a trace-only binary for ChampSim etc.
 */
#ifdef BENCH_VERBOSE
#  define BENCH_PRINTF(...)  printf(__VA_ARGS__)
#else
#  define BENCH_PRINTF(...)  do { } while (0)
#endif

/*
 * TRACE_MODE:
 *   - When defined, we do NOT initialize B in order to avoid large init cost.
 *   - Only A is initialized. This is intended for trace-only builds where the
 *     numeric values of B do not matter (only the address pattern matters).
 */
#ifdef TRACE_MODE
#warning "Compiling with TRACE_MODE: B array is NOT initialized (trace-only build)"
#endif

// Global sink to prevent the compiler from optimizing the kernel away.
static volatile double sink = 0.0;

/*
 * Initialize an array with simple increasing values so that the compiler
 * cannot easily optimize away the loads.
 */
static void init_array(double *p, size_t n, double base)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = base + (double)i * 0.000001;
    }
}

/*
 * Kernel:
 *   - outer_iters = B_elems / elems_per_iter (fixed for given B_bytes, chunk_bytes)
 *   - For each outer iteration:
 *       1) Sweep entire A once (intended to disturb / thrash L1).
 *       2) Access one "chunk" of B, consisting of elems_per_iter elements,
 *          using the given stride_elems.
 *
 *   Logical B coverage:
 *     base index for this chunk = outer * elems_per_iter * stride_elems
 *     then we access:
 *       B[base + 0 * stride_elems],
 *       B[base + 1 * stride_elems],
 *       ...
 *       B[base + (elems_per_iter - 1) * stride_elems]
 *
 *   - If stride_elems = 1 (dense), we simply sweep B in order.
 *   - If stride_elems > 1 (strided), we jump by that stride within each chunk.
 */
static double run_kernel(double *A, double *B,
                         size_t A_elems,
                         size_t B_elems,      // logical B size when stride=1
                         size_t elems_per_iter,
                         size_t stride_elems)
{
    // Number of outer-loop iterations (independent of stride)
    size_t outer_iters = B_elems / elems_per_iter;

    double sum = 0.0;

    for (size_t outer = 0; outer < outer_iters; outer++) {
        // 1) Sweep entire A to disturb / thrash L1
        for (size_t i = 0; i < A_elems; i++) {
            sum += A[i];
        }

        // 2) Access one chunk of B
        size_t base = outer * elems_per_iter * stride_elems;

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

    int    access_mode = 0;  // 0 = dense, 1 = strided
    size_t user_stride = 8;  // also used to size the B allocation
    size_t outer_scale = 1;  // how many times to call run_kernel

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

    // Treat dense mode as stride=1 inside the kernel (no branch inside run_kernel).
    size_t stride_elems = (access_mode == 0) ? 1 : user_stride;

    size_t A_elems        = A_bytes     / sizeof(double);
    size_t B_elems        = B_bytes     / sizeof(double);  // logical B size
    size_t elems_per_iter = chunk_bytes / sizeof(double);

    if (A_elems == 0 || B_elems == 0 || elems_per_iter == 0) {
        fprintf(stderr, "A_bytes, B_bytes, chunk_bytes must be >= sizeof(double)\n");
        return 1;
    }
    if (B_elems % elems_per_iter != 0) {
        fprintf(stderr, "B_bytes must be a multiple of chunk_bytes.\n");
        return 1;
    }

    /*
     * B allocation size:
     *
     * For one run_kernel call, the required B elements are:
     *   required = outer_iters * elems_per_iter * stride_elems
     *            = (B_elems / elems_per_iter) * elems_per_iter * stride_elems
     *            = B_elems * stride_elems
     *
     * We allocate:
     *   B_elems_alloc = B_elems * user_stride
     *
     * - access_mode == 0 (dense):
     *     stride_elems = 1, required = B_elems
     *     B_elems_alloc = B_elems * user_stride >= required
     *
     * - access_mode == 1 (strided):
     *     stride_elems = user_stride, required = B_elems * user_stride
     *     B_elems_alloc matches required.
     *
     * outer_scale only affects how many times we call run_kernel and does not
     * require extra memory (we reuse the same region multiple times).
     */
    size_t B_elems_alloc = B_elems * user_stride;

    // For debug logging (if BENCH_VERBOSE is enabled)
    size_t base_outer_iters  = B_elems / elems_per_iter;
    size_t total_outer_iters = base_outer_iters * outer_scale;

    #ifndef BENCH_VERBOSE
        (void)total_outer_iters;  // suppress unused-variable warning when silent
    #endif

    BENCH_PRINTF("# Params:\n");
    BENCH_PRINTF("#   A_bytes        = %zu\n", A_bytes);
    BENCH_PRINTF("#   B_bytes        = %zu\n", B_bytes);
    BENCH_PRINTF("#   chunk_bytes    = %zu\n", chunk_bytes);
    BENCH_PRINTF("#   A_elems        = %zu\n", A_elems);
    BENCH_PRINTF("#   B_elems        = %zu\n", B_elems);
    BENCH_PRINTF("#   B_elems_alloc  = %zu  (allocated)\n", B_elems_alloc);
    BENCH_PRINTF("#   elems_per_iter = %zu\n", elems_per_iter);
    BENCH_PRINTF("#   access_mode    = %d (0=dense,1=strided)\n", access_mode);
    BENCH_PRINTF("#   user_stride    = %zu (for allocation)\n", user_stride);
    BENCH_PRINTF("#   stride_elems   = %zu (effective in kernel)\n", stride_elems);
    BENCH_PRINTF("#   base_outer_iters = %zu (per run_kernel)\n", base_outer_iters);
    BENCH_PRINTF("#   outer_scale      = %zu (run_kernel repeats)\n", outer_scale);
    BENCH_PRINTF("#   total_outer_iters = %zu (base_outer_iters * outer_scale)\n",
                 total_outer_iters);

#ifdef TRACE_MODE
    BENCH_PRINTF("#   TRACE_MODE: B is not initialized (values arbitrary, address pattern only)\n");
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
    // Normal build: initialize both A and B for correct numeric behavior / perf.
    init_array(A, A_elems, 1.0);
    init_array(B, B_elems_alloc, 1000.0);
#else
    // Trace-only build: only A is initialized; B is left as-is.
    init_array(A, A_elems, 1.0);
#endif

    double sum = 0.0;

    // Repeat the same kernel outer_scale times.
    // (Instruction stream is the same; we just extend runtime to gather statistics.)
    for (size_t rep = 0; rep < outer_scale; rep++) {
        sum += run_kernel(A, B,
                          A_elems,
                          B_elems,
                          elems_per_iter,
                          stride_elems);
    }

    // Prevent the compiler from optimizing away the whole computation.
    sink = sum;

    BENCH_PRINTF("sum = %.6f\n", sum);

    free(A);
    free(B);
    return 0;
}
