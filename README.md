
# wrongpath-bench

A tiny, parameterized memory microbenchmark plus a `perf` wrapper script.

The goal is to emulate the “large tail B after a loop A” pattern and measure
how often we actually go out to DRAM, in preparation for wrong-path prefetch
experiments.

- Array **A**: small-ish array that we scan every outer iteration to blow out L1.
- Array **C**: backing store for a logical “B array” that is big enough to
  exceed L2 / LLC and cause DRAM traffic.
- Two access patterns for the tail:
  - **dense**: sequential access inside each chunk
  - **strided**: fixed-element stride inside each chunk, with a larger
    effective working set

`perf` is used to measure L1 / L2 behavior and DRAM demand fills and to report
per-1K-instruction metrics (MPKI / PKI).

Tested mainly on AMD server CPUs (e.g., TAMU clusters); the event names are
AMD-specific and may need adjustment on other platforms.

---

## Files

- `benchmark.c`  
  C microbenchmark that implements the A + B pattern and exposes several
  knobs:
  - size of A
  - logical size of B
  - chunk size
  - dense vs strided access
  - stride length
  - optional outer repetition count for the kernel body

- `scripts/run_perf_mpki.py`  
  Helper script that runs `perf stat` on `benchmark` and parses:
  - L1 D-cache miss rate and MPKI
  - L2 miss rate and MPKI (on L1D misses)
  - DRAM demand-fill PKI (fills per kilo-instruction)

---

## Build

Simple build:

```bash
gcc -O3 -march=native -Wall -o benchmark benchmark.c
````

You can of course substitute `clang` or adjust flags as needed.

Requirements:

* A recent `gcc` or `clang`
* Linux `perf` installed and usable
* Sufficient permissions to use hardware performance counters
  (e.g. `perf_event_paranoid` not too strict)

---

## Benchmark design

### High-level picture

* **A array**

  * Size = `A_bytes`
  * Access pattern: linear scan `A[0 .. A_elems-1]` every outer iteration
  * Role: blow out L1 so that the following B/C accesses are not trivially
    hitting in L1

* **Logical B**

  * Logical size = `B_bytes`
  * Determines how many “chunks” we have:

    * `logical_B_elems = B_bytes / sizeof(double)`
    * `chunk_elems     = chunk_bytes / sizeof(double)`
    * `outer_iters     = logical_B_elems / chunk_elems`
  * We conceptually think of B as `outer_iters` chunks, each of `chunk_bytes`.

* **Physical C**

  * Actually allocated array; backing store for logical B
  * Size:

    * `C_elems = logical_B_elems * user_stride`
  * This is large enough so that strided mode can spread accesses out more
    aggressively than dense mode, while keeping the loop structure the same.

### Access pattern

The kernel is:

```c
static double run_kernel(double *A, double *C,
                         size_t A_elems,
                         size_t logical_B_elems,
                         size_t chunk_elems,
                         size_t stride_elems,
                         size_t kernel_reps)
{
    size_t outer_iters = logical_B_elems / chunk_elems;
    double sum = 0.0;

    for (size_t rep = 0; rep < kernel_reps; rep++) {
        for (size_t outer = 0; outer < outer_iters; outer++) {
            // 1) Sweep A to thrash L1
            for (size_t i = 0; i < A_elems; i++) {
                sum += A[i];
            }

            // 2) Access one chunk of logical B via C
            size_t base = outer * chunk_elems * stride_elems;

            for (size_t j = 0; j < chunk_elems; j++) {
                size_t idx = base + j * stride_elems;
                sum += C[idx];
            }
        }
    }

    return sum;
}
```

* In **dense mode**:

  * `stride_elems = 1`
  * C is still allocated at `logical_B_elems * user_stride`, but we only use a
    contiguous subset that corresponds to dense B.

* In **strided mode**:

  * `stride_elems = user_stride` (e.g., 8, 16, 32)
  * For each logical chunk, we walk C with that stride, so the effective
    working set per chunk is `chunk_elems * stride_elems * sizeof(double)`.
  * Because we keep `outer_iters` and `chunk_elems` the same, the dynamic loop
    structure (and thus instruction count) is roughly comparable between dense
    and strided runs.

---

## Command-line interface

The `benchmark` binary has the following usage:

```text
./benchmark A_bytes B_bytes chunk_bytes [access_mode] [user_stride] [kernel_reps]

  A_bytes      : Size of array A in bytes.
  B_bytes      : Logical "B" size in bytes.
  chunk_bytes  : Size of each chunk of B in bytes.
  access_mode  : 0 = dense, 1 = strided (default: 0).
  user_stride  : Stride in elements used for allocation and strided mode
                 (default: 8).
  kernel_reps  : Number of times to repeat the whole kernel body
                 (default: 1).
```

Details:

* `A_bytes`

  * Choose something around / above the L1D size.
  * In many of the examples we use `A_bytes = 32 * 1024`.

* `B_bytes`

  * Logical size of B.
  * In our experiments we often use `B_bytes = 512 * 1024 * 1024` (512 MiB) to
    ensure we go well beyond LLC and get DRAM traffic.

* `chunk_bytes`

  * Logical per-iteration footprint from B.
  * We commonly use `chunk_bytes = 512 * 1024`.

* `access_mode`

  * `0` (dense): `stride_elems` is forced to 1 internally.
  * `1` (strided): `stride_elems = user_stride`.

* `user_stride`

  * Used in two places:

    * `C_elems = logical_B_elems * user_stride`
      → actual backing array size
    * `stride_elems` when `access_mode=1`.
  * This way dense and strided runs allocate the same C size, and differ only
    in how they walk it.

* `kernel_reps`

  * Wraps the whole kernel in an outer repetition loop.
  * Useful when you want to make the kernel region “heavy” enough that
    initialization overhead is negligible in `perf` output.
  * If you only care about the steady-state kernel, you can bump this to 10,
    100, etc., and look mainly at MPKI/PKI.

---

## Typical parameter choices

On the current AMD server testbed we often use:

```bash
# 32 KiB A, 512 MiB logical B, 512 KiB chunks
A_BYTES=$((32*1024))
B_BYTES=$((512*1024*1024))
CHUNK_BYTES=$((512*1024))

# Dense (sequential B)
./benchmark $A_BYTES $B_BYTES $CHUNK_BYTES 0 8

# Strided B, stride = 8 elements
./benchmark $A_BYTES $B_BYTES $CHUNK_BYTES 1 8

# Same, but repeat the kernel 100 times
./benchmark $A_BYTES $B_BYTES $CHUNK_BYTES 1 8 100
```

These correspond roughly to:

* A: L1-sized thrash array.
* B: large tail that goes well beyond LLC.
* Chunk: medium granularity (around L2-sized), so we can talk about “per-chunk
  coverage” in the context of wrong-path prefetch.

---

## `perf` wrapper: `run_perf_mpki.py`

The Python script wraps `perf stat` and computes the metrics we care about.

### Events

It currently uses the following events (AMD terminology):

```python
EVENTS = [
    "instructions",
    "L1-dcache-loads",
    "L1-dcache-load-misses",
    "l2_cache_accesses_from_dc_misses",
    "l2_cache_misses_from_dc_misses",
    "ls_refills_from_sys.ls_mabresp_lcl_dram",
    "ls_refills_from_sys.ls_mabresp_rmt_dram",
]
```

Rough meaning:

* `instructions`
  Total retired instructions (used as the denominator for MPKI/PKI).

* `L1-dcache-loads`, `L1-dcache-load-misses`
  Load accesses and the associated misses / fills in L1D.

* `l2_cache_accesses_from_dc_misses`
  L2 accesses that originate from L1D misses (including prefetches).

* `l2_cache_misses_from_dc_misses`
  Subset of the above that miss in L2 as well.

* `ls_refills_from_sys.ls_mabresp_lcl_dram`
  Demand D-cache fills that are sourced from local DRAM / IO.

* `ls_refills_from_sys.ls_mabresp_rmt_dram`
  Demand D-cache fills from remote DRAM / IO.

The last two together are our “DRAM demand fills”.

If your CPU has different event names, edit `EVENTS` to match your `perf list`
output.

### Usage

From the repo root:

```bash
./scripts/run_perf_mpki.py ./benchmark 32768 536870912 524288 1 8
```

You can pass any additional benchmark arguments after the binary name, exactly
the same as when calling `./benchmark` directly.

### Output format

The script prints:

1. **Raw `perf` CSV output** (stderr), so you can copy it into a log or parse
   it later if needed:

   ```text
   === perf raw output (stderr) ===
   4651578578,,instructions:u,631451314,71.31,,
   89461602,,L1-dcache-loads:u,631901862,71.37,,
   134683770,,L1-dcache-load-misses:u,632899224,71.48,150.55,of all L1-dcache accesses
   ...
   ```

2. **Parsed counters**:

   ```text
   === Parsed counters ===
   instructions            : 4651578578
   L1-dcache-loads         : 89461602
   L1-dcache-load-misses   : 134683770
   l2_cache_accesses_from_dc_misses : 134746203
   l2_cache_misses_from_dc_misses   : 14091861
   DRAM fills (local+remote): 57975543 (local=57975527, remote=16)
   ```

3. **Rates**:

   ```text
   === Rates ===
   L1 miss rate            : 150.55 %
   L2 miss rate (on L1D misses): 10.46 %
   ```

   Note: the L1 “miss rate” can exceed 100% because the event definition counts
   fills in a way that is not strictly “misses / loads” in the textbook sense
   (prefetch, write-alloc, etc.). We treat it as “L1D fills per L1D access.”

4. **Per-1K-instruction metrics** (MPKI / PKI):

   ```text
   === Per-1K-instruction metrics (MPKI/PKI) ===
   L1 MPKI                 : 28.954
   L2 MPKI                 : 3.029
   DRAM fill PKI (local+remote): 12.464
   ```

   Definitions:

   * **L1 MPKI** = `L1-dcache-load-misses / (instructions / 1000)`
   * **L2 MPKI** = `l2_cache_misses_from_dc_misses / (instructions / 1000)`
   * **DRAM fill PKI** =
     `(ls_refills_from_sys.ls_mabresp_lcl_dram + ls_refills_from_sys.ls_mabresp_rmt_dram) / (instructions / 1000)`

   We call the last one **PKI** (“fills per kilo-instruction”) instead of MPKI
   because they are fills from DRAM, not cache misses in the strict sense.

---

## How this ties into wrong-path prefetch experiments

The eventual experiment is:

* Introduce a wrong-path / loop-tail prefetch mechanism (e.g., via modified
  trace or simulator).
* Compare:

  * **DRAM fill PKI** before vs after
    → demand DRAM fills should go down.
  * Breakdown between demand vs prefetch-driven DRAM activity (future work:
    add HW/SW prefetch events).
  * L1 / L2 MPKI and IPC.

This microbenchmark and `run_perf_mpki.py` give a controlled environment where:

* The loop body is simple and reproducible.
* You can switch between dense vs strided tail accesses and adjust stride
  length.
* You can quickly see how changes in access pattern affect:

  * L1/L2 MPKI
  * DRAM demand fill PKI

---

## Caveats / TODO

* Event names are AMD-specific; for Intel or other vendors, the `EVENTS` list
  will need to be adapted.
* The benchmark does not try to eliminate all sources of variation
  (TLB behavior, page mapping, NUMA placement, etc.).
* For very small `kernel_reps`, initialization costs may still dilute the
  measured MPKI/PKI. Increase `kernel_reps` when in doubt.

Pull requests or local branches that add:

* instruction-only region timing,
* more detailed prefetch breakdown,
* or multi-thread variants

are welcome.

