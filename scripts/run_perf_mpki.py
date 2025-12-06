#!/usr/bin/env python3
import argparse
import subprocess
import sys
import socket

# ==============================
# 設定
# ==============================

EVENTS = [
    "cycles",
    "instructions",
    "L1-dcache-loads",
    "L1-dcache-load-misses",
    "l2_cache_accesses_from_dc_misses",
    "l2_cache_misses_from_dc_misses",
    "ls_refills_from_sys.ls_mabresp_lcl_dram",
    "ls_refills_from_sys.ls_mabresp_rmt_dram",
]

# 実行ノード名（FQDN のまま。短くしたければ .split('.')[0] でもよい）
NODE = socket.gethostname()


# ==============================
# perf stderr パース
# ==============================

def parse_perf_stderr(stderr: str):
    """
    perf stat -x, の stderr をパースして
    event_name -> count の辞書を返す。

    例:
      665505349,,cycles:u,173811319,62.35,,
    """
    counters = {}

    for line in stderr.splitlines():
        line = line.strip()
        if not line:
            continue

        parts = line.split(",")
        if len(parts) < 3:
            continue

        value_str = parts[0].strip()
        event_name_raw = parts[2].strip()

        # ヘッダや空行はスキップ
        if not value_str or not event_name_raw:
            continue

        # value が整数じゃない行（エラー行など）はスキップ
        try:
            val = int(value_str)
        except ValueError:
            continue

        # ★ここが重要★
        # cycles:u → cycles,  instructions:k → instructions みたいに正規化
        event_name = event_name_raw.split(":")[0]

        counters[event_name] = val

    return counters


# ==============================
# メイン処理
# ==============================

def main():
    parser = argparse.ArgumentParser(
        description="Run perf stat on benchmark and print IPC / MPKI summary."
    )
    parser.add_argument(
        "benchmark",
        help="path to benchmark binary (e.g., ./benchmark)",
    )
    parser.add_argument(
        "bench_args",
        nargs=argparse.REMAINDER,
        help="arguments passed to the benchmark (A_bytes B_bytes chunk_bytes ...)",
    )

    args = parser.parse_args()

    bench = args.benchmark
    bench_args = args.bench_args

    cmd = [
        "perf", "stat",
        "-x,",                # CSV 形式
        "-e", ",".join(EVENTS),
        "--",
        bench,
    ] + bench_args

    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        check=False,
    )

    stdout = proc.stdout
    stderr = proc.stderr

    print("=== perf raw output (stderr) ===")
    print(stderr.strip())
    print()

    if proc.returncode != 0:
        print("perf stat exited with non-zero status:", proc.returncode, file=sys.stderr)

    counters = parse_perf_stderr(stderr)

    cycles        = counters.get("cycles", 0)
    instructions  = counters.get("instructions", 0)
    l1_loads      = counters.get("L1-dcache-loads", 0)
    l1_misses     = counters.get("L1-dcache-load-misses", 0)
    l2_accesses   = counters.get("l2_cache_accesses_from_dc_misses", 0)
    l2_misses     = counters.get("l2_cache_misses_from_dc_misses", 0)
    dram_local    = counters.get("ls_refills_from_sys.ls_mabresp_lcl_dram", 0)
    dram_remote   = counters.get("ls_refills_from_sys.ls_mabresp_rmt_dram", 0)
    dram_total    = dram_local + dram_remote

    print("=== Parsed counters ===")
    print("node                    : {}".format(NODE))
    print("cycles                 : {}".format(cycles))
    print("instructions           : {}".format(instructions))
    print("L1-dcache-loads        : {}".format(l1_loads))
    print("L1-dcache-load-misses  : {}".format(l1_misses))
    print("l2_cache_accesses_from_dc_misses : {}".format(l2_accesses))
    print("l2_cache_misses_from_dc_misses   : {}".format(l2_misses))
    print("Demand DRAM fills (L1D): {} (local={}, remote={})".format(
        dram_total, dram_local, dram_remote
    ))
    print()

    print("=== Rates / IPC ===")
    if l1_loads > 0:
        l1_miss_rate = 100.0 * l1_misses / l1_loads
    else:
        l1_miss_rate = 0.0

    if l2_accesses > 0:
        l2_miss_rate = 100.0 * l2_misses / l2_accesses
    else:
        l2_miss_rate = 0.0

    if cycles > 0:
        ipc = float(instructions) / float(cycles)
    else:
        ipc = 0.0

    print("L1 miss rate           : {:.2f} %".format(l1_miss_rate))
    print("L2 miss rate (on L1D misses): {:.2f} %".format(l2_miss_rate))
    print("IPC                    : {:.3f}".format(ipc))
    print()

    print("=== Per-1K-instruction metrics (MPKI/PKI) ===")
    if instructions > 0:
        l1_mpki  = 1000.0 * l1_misses / instructions
        l2_mpki  = 1000.0 * l2_misses / instructions
        dram_pki = 1000.0 * dram_total / instructions
    else:
        l1_mpki = l2_mpki = dram_pki = 0.0

    print("L1 MPKI                : {:.3f}".format(l1_mpki))
    print("L2 MPKI                : {:.3f}".format(l2_mpki))
    print("Demand DRAM fills (L1D) PKI : {:.3f}".format(dram_pki))
    print()

    if stdout.strip():
        print("=== benchmark stdout ===")
        print(stdout.strip())


if __name__ == "__main__":
    main()
