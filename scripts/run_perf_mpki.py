#!/usr/bin/env python3
import argparse
import subprocess
import sys
import socket

# ==============================
# 設定
# ==============================

# 使う perf イベント
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

# ホスト名（ここが修正ポイント：固定文字列ではなく hostname から取得）
NODE = socket.gethostname()  # FQDN のままで出す。短くしたければ split(".")[0]


# ==============================
# パーサ
# ==============================

def parse_perf_stderr(stderr: str):
    """
    perf stat -x, の stderr をパースして
    event_name -> count の辞書を返す。
    """
    counters = {}

    for line in stderr.splitlines():
        line = line.strip()
        if not line:
            continue
        # perf stat -x, の行は "value, ,event,..." みたいな形式
        parts = line.split(",")
        if len(parts) < 3:
            continue

        value_str = parts[0].strip()
        event_name = parts[2].strip()

        # ヘッダやエラー行はスキップ
        if not value_str or not event_name:
            continue

        # カウントを int 変換（失敗したら無視）
        try:
            val = int(value_str)
        except ValueError:
            continue

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

    # perf コマンド組み立て
    cmd = [
        "perf", "stat",
        "-x,",                # CSV っぽく
        "-e", ",".join(EVENTS),
        "--",
        bench,
    ] + bench_args

    # 実行
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        check=False,   # 失敗時も stderr を見たいので自前で判定
    )

    stdout = proc.stdout
    stderr = proc.stderr

    # 生の stderr をまず出す
    print("=== perf raw output (stderr) ===")
    print(stderr.strip())
    print()

    if proc.returncode != 0:
        print("perf stat exited with non-zero status:", proc.returncode, file=sys.stderr)

    # パース
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

    # === ここからサマリ出力 ===
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

    # レート / IPC
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

    # MPKI / PKI
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

    # ベンチ stdout も最後にそのまま吐いておきたい場合
    if stdout.strip():
        print("=== benchmark stdout ===")
        print(stdout.strip())


if __name__ == "__main__":
    main()
