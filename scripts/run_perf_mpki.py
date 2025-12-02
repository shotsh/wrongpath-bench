#!/usr/bin/env python3
import argparse
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser(
        description="Run perf stat and compute L1/L2/DRAM MPKI."
    )
    # 以降の引数はそのまま実行コマンドとして渡す
    parser.add_argument(
        "cmd",
        nargs=argparse.REMAINDER,
        help="command to run, e.g. ./benchmark 32768 536870912 524288 0",
    )
    args = parser.parse_args()

    if not args.cmd:
        parser.error("you must specify the command to run, e.g. ./benchmark ...")

    perf_cmd = [
        "perf", "stat", "-x,",           # CSV 風フォーマット
        "-e", "instructions",
        "-e", "L1-dcache-loads",
        "-e", "L1-dcache-load-misses",
        "-e", "l2_cache_accesses_from_dc_misses",
        "-e", "l2_cache_misses_from_dc_misses",
        "-e", "ls_refills_from_sys.ls_mabresp_lcl_dram",
        "-e", "ls_refills_from_sys.ls_mabresp_rmt_dram",
        "--",
    ] + args.cmd

    # perf の stderr にカウンタが出るのでそれをキャプチャ
    proc = subprocess.run(
        perf_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,  # こっちに変更
    )

    # 元の出力も一応表示
    print("=== perf raw output (stderr) ===")
    print(proc.stderr)

    if proc.returncode != 0 and "Performance counter stats" not in proc.stderr:
        print(f"perf failed with code {proc.returncode}", file=sys.stderr)
        sys.exit(proc.returncode)

    # perf stat -x, の 1 行はだいたい:
    # <value>,<unit>,<event>,<run_count>,<metric>
    values = {}

    for line in proc.stderr.splitlines():
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 3:
            continue
        val_str, _, name = parts[:3]
        if not name:
            continue
        values[name] = val_str

    def get_val(name: str) -> int:
        v = values.get(name)
        if v is None:
            return 0
        # "<not supported>" とか "<not counted>" は 0 扱い
        if v.startswith("<"):
            return 0
        try:
            return int(v)
        except ValueError:
            return 0

    instr      = get_val("instructions")
    l1_loads   = get_val("L1-dcache-loads")
    l1_miss    = get_val("L1-dcache-load-misses")
    l2_acc     = get_val("l2_cache_accesses_from_dc_misses")
    l2_miss    = get_val("l2_cache_misses_from_dc_misses")
    dram_loc   = get_val("ls_refills_from_sys.ls_mabresp_lcl_dram")
    dram_rmt   = get_val("ls_refills_from_sys.ls_mabresp_rmt_dram")
    dram_total = dram_loc + dram_rmt

    if instr == 0:
        print("instructions is 0, cannot compute MPKI", file=sys.stderr)
        sys.exit(1)

    # MPKI 計算
    l1_mpki   = l1_miss   * 1000.0 / instr
    l2_mpki   = l2_miss   * 1000.0 / instr
    dram_mpki = dram_total * 1000.0 / instr

    # miss rate も一緒に計算
    if l1_loads > 0:
        l1_miss_rate = 100.0 * l1_miss / l1_loads
    else:
        l1_miss_rate = None

    if l2_acc > 0:
        l2_miss_rate = 100.0 * l2_miss / l2_acc
    else:
        l2_miss_rate = None

    print()
    print("=== Derived metrics (per 1K instructions) ===")
    print(f"instructions               : {instr}")
    print()
    print(f"L1D misses   = {l1_miss}  (loads = {l1_loads})")
    print(f"  -> L1D MPKI  = {l1_mpki:.3f}")
    if l1_miss_rate is not None:
        print(f"  -> L1D miss% = {l1_miss_rate:.2f} %")
    else:
        print("  -> L1D miss% = N/A")
    print()
    print(f"L2 misses(from L1D misses) = {l2_miss}  (accesses = {l2_acc})")
    print(f"  -> L2 MPKI                = {l2_mpki:.3f}")
    if l2_miss_rate is not None:
        print(f"  -> L2 miss%               = {l2_miss_rate:.2f} %")
    else:
        print("  -> L2 miss%               = N/A")
    print()
    print(f"DRAM refills (local+remote) = {dram_total}  "
          f"(local={dram_loc}, remote={dram_rmt})")
    print(f"  -> DRAM MPKI              = {dram_mpki:.3f}")

if __name__ == "__main__":
    main()
