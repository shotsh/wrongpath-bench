# wrongpath-bench

This is a small microbenchmark that uses a loop body plus a "large array access" so that demand accesses to the cache hierarchy and DRAM can be observed in a controllable way.

The main goals are:

* To roughly understand **L1/L2/DRAM miss frequency (MPKI/PKI)**.
* To switch **access patterns (dense / strided)** and see how
  * demand accesses that reach DRAM, and
  * L1/L2 hit rates  
    change.
* In the future, to use this as the basis for **trace generation for wrong-path prefetch / loop-tail experiments**.

A Python script that wraps `perf stat` can automatically compute MPKI and DRAM fill PKI.

---

## File layout

* `benchmark.c`  
  Loop body that accesses a small array `A` which blows out L1 and a large array `B`.
* `scripts/run_perf_mpki.py`  
  Wrapper that calls `perf stat` and computes and prints L1/L2 miss rates, MPKI, DRAM fill PKI, and IPC.

---

## How to build

You can just use `gcc`.

```bash
gcc -O3 -march=native -Wall -o benchmark benchmark.c
````

---

## Benchmark behavior

### Command line arguments

```bash
./benchmark A_bytes B_bytes chunk_bytes [access_mode] [stride_elems] [outer_scale]
```

* `A_bytes`

  * Size of the small array `A` in bytes.
  * On every iteration, we **scan all elements** and blow out L1.

* `B_bytes`

  * Logical size of `B` in bytes.
  * This divided by `chunk_bytes` determines the **outer loop count**.

* `chunk_bytes`

  * Size (in bytes) used to cut B into "chunks".
  * `B_bytes` must be a multiple of `chunk_bytes`.

* `access_mode` (optional, default 0)

  * `0` = dense access (contiguous access)
  * `1` = strided access

* `stride_elems` (optional, default 8)

  * Stride when `access_mode=1` (in units of double elements).
  * Even when `access_mode=0`, it still affects the **allocation size of array B** (see below).

* `outer_scale` (optional, default 1)

  * How many times to run the entire loop.
  * A knob that scales the **effective outer loop count**.

### Array sizes and loop counts

The main parameters in the code are:

* `A_elems = A_bytes / sizeof(double)`
* `B_elems = B_bytes / sizeof(double)`  … logical number of elements in B
* `chunk_elems = chunk_bytes / sizeof(double)`

Constraints:

* `A_elems > 0`
* `B_elems > 0`
* `chunk_elems > 0`
* `B_elems % chunk_elems == 0`

When the logical B is divided into chunks:

```text
base_outer_iters = B_elems / chunk_elems
```

Then we scale everything with `outer_scale`:

```text
outer_iters = base_outer_iters * outer_scale
```

The actual loops in the kernel are:

```c
for (outer = 0; outer < outer_iters; outer++) {
    // Sweep the entire A to blow out L1
    for (i = 0; i < A_elems; i++) { ... }

    size_t base = outer * chunk_elems * stride_elems;

    // Always loop exactly chunk_elems times
    for (j = 0; j < chunk_elems; j++) {
        size_t idx = base + j * stride_elems;
        sum += B[idx];
    }
}
```

### Switching between dense and strided

* When `access_mode == 0` (dense):

  * `stride_elems` is effectively fixed to 1.
  * However, `user_stride` still affects the allocation size of B.

* When `access_mode == 1` (strided):

  * `stride_elems = user_stride` is used as is.

In summary:

* The logical B size (`B_bytes`) and `chunk_bytes` determine the **outer and inner loop counts**.
* Changing only `stride_elems` means:

  * The outer and inner loop counts stay the same.
  * The **number of executed instructions is almost the same**.
  * Only the access pattern and working set change.

### Size of B allocation

The actual number of elements allocated for B is

```c
B_elems_alloc = B_elems * user_stride * outer_scale;
```

* In the dense case (`access_mode=0`):

  * Access uses `stride_elems = 1`.
  * But we allocate **with slack**, multiplied by `user_stride` and `outer_scale`.

* In the strided case (`access_mode=1`):

  * `stride_elems = user_stride`.
  * The required size is `B_elems * user_stride * outer_scale`, which exactly matches `B_elems_alloc`.

The reasons for this design are:

* Keep the **same loop counts and same function**.
* Change only `stride_elems` so that:

  * The number of instructions is almost unchanged.
  * Only the memory access pattern and the rate of reaching DRAM changes, which makes comparison easier.

---

## Typical parameter examples

* `A_bytes`

  * Slightly larger than the core's L1D.
  * Example: if L1D = 32 KiB, use `A_bytes = 32*1024`.

* `B_bytes`

  * Make it much larger than the LLC so that you reach DRAM often enough.
  * Example: `B_bytes = 512*1024*1024` (512 MiB).

* `chunk_bytes`

  * Around the L2 size, or a subset of it.
  * Example: `chunk_bytes = 512*1024` (512 KiB).

* `access_mode` / `stride_elems`

  * Dense: `access_mode = 0`, `stride_elems` can be anything (it only affects allocation size).
  * Strided: `access_mode = 1`, for example:

    * `stride_elems = 8` (about 1 element per cache line)
    * `stride_elems = 16`, etc.

* `outer_scale`

  * Use a larger value when you want to emphasize the IPC / MPKI of the loop body.
  * Example: `outer_scale = 100`.

---

## perf wrapper script (`run_perf_mpki.py`)

### Overview

This script:

* Calls `perf stat -x,` to collect CSV output.
* Parses the following counters:

  * `cycles`
  * `instructions`
  * `L1-dcache-loads`
  * `L1-dcache-load-misses`
  * `l2_cache_accesses_from_dc_misses`
  * `l2_cache_misses_from_dc_misses`
  * DRAM related events (these differ by node)
* And from these, it computes and displays:

  * L1 miss rate
  * L2 miss rate (using L1D misses as the denominator)
  * L1 / L2 MPKI
  * DRAM fill PKI (demand fills per 1K instructions)
  * IPC

### How to use

Format:

```bash
./scripts/run_perf_mpki.py [--node demeter|artemis] <binary> [binary-args...]
```

#### Example 1: n05-demeter (EPYC, node with `ls_refills_from_sys.*`)

If you omit `--node`, it runs in `demeter` mode.

```bash
./scripts/run_perf_mpki.py ./benchmark 32768 536870912 524288 1 16 100
```

In this mode, DRAM fills are counted as the sum of:

* `ls_refills_from_sys.ls_mabresp_lcl_dram`
* `ls_refills_from_sys.ls_mabresp_rmt_dram`

#### Example 2: n07-artemis (only `ls_dmnd_fills_from_sys.mem_io_local` available)

```bash
./scripts/run_perf_mpki.py --node artemis ./benchmark 32768 536870912 524288 1 16 100
```

In this mode, DRAM fills are treated as:

* Use `ls_dmnd_fills_from_sys.mem_io_local` **as the local part**.
* Treat remote as 0.

### Output format

1. Raw stderr from `perf` (CSV)
2. Parsed counter list
3. Miss rates and IPC
4. MPKI / PKI

Example:

```text
=== perf raw output (stderr) ===
... (perf output as is) ...

=== Parsed counters ===
node                    : demeter
cycles                 : 1921158234
instructions           : 4649560413
L1-dcache-loads        : 78971700
L1-dcache-load-misses  : 134840330
l2_cache_accesses_from_dc_misses : 134891535
l2_cache_misses_from_dc_misses   : 13975630
Demand DRAM fills (L1D): 57480933 (local=57480933, remote=0)

=== Rates / IPC ===
L1 miss rate           : 170.75 %
L2 miss rate (on L1D misses): 10.36 %
IPC                    : 2.420

=== Per-1K-instruction metrics (MPKI/PKI) ===
L1 MPKI                : 28.954
L2 MPKI                : 3.029
Demand DRAM fills (L1D) PKI : 12.464
```

Here:

* **L1 MPKI / L2 MPKI**

  * `L1/L2 misses per 1000 instructions`.

* **DRAM fill PKI**

  * The sum of `ls_refills_from_sys...` or `ls_dmnd_fills_from_sys...` interpreted as
    "demand DRAM fill count per 1000 instructions".

---

## What to look at

* **L1 MPKI / L2 MPKI**

  * Normally increases when you increase the stride.
  * When you increase `outer_scale`, the effect of initialization is reduced and the **miss characteristics of the loop body** show up more clearly.

* **DRAM fill PKI**

  * A rough indicator of "how much this loop is hitting DRAM".
  * For wrong-path prefetch evaluation:

    * The absolute number of DRAM fills may not change much, but
    * You want to see how the **ratio of demand vs prefetch** changes.
      This benchmark serves as a baseline for that.

---

## Caveats and limitations

* This is a **very simple streaming-style microbenchmark**.

  * It hardly includes factors that matter in real OS or applications, such as TLB behavior, snoops, coherence, and virtualization.

* `perf` event names differ by CPU generation and kernel, so:

  * When you use a new node, you should check `perf list` first, and
  * Assume you may need to extend the event definitions in `run_perf_mpki.py`.

* Depending on the value of `perf_event_paranoid`, the kernel may restrict perf and you may not be able to use it at all.

---

If you use this README as a base, it should make it much easier later to:

* Check assumptions when you wonder "are these numbers strange?"
* Do porting work when you bring this to a new node.

If needed, you can also add a table of "recommended parameter sets (dense/stride × outer_scale)" on top of this.

```
```
