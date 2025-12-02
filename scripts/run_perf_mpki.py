#!/usr/bin/env python3
import subprocess
import sys


EVENTS = [
    "instructions",
    "L1-dcache-loads",
    "L1-dcache-load-misses",
    "l2_cache_accesses_from_dc_misses",
    "l2_cache_misses_from_dc_misses",
    "ls_refills_from_sys.ls_mabresp_lcl_dram",
    "ls_refills_from_sys.ls_mabresp_rmt_dram",
]


def run_perf(cmd):
    perf_cmd = [
        "perf", "stat",
        "-x,",  # CSV っぽい区切り
    ]
    for ev in EVENTS:
        perf_cmd += ["-e", ev]
    perf_cmd.append("--")
    perf_cmd += cmd

    # Python 3.6 用: text= ではなく universal_newlines=
    proc = subprocess.run(
        perf_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )

    return proc.returncode, proc.stdout, proc.stderr


def parse_perf_csv(stderr_text):
    """
    perf stat -x, の stderr から
    { イベント名(正規化) -> 値(int) } の dict を作る
    """
    counters = {}

    for line in stderr_text.splitlines():
        line = line.strip()
        if not line:
            continue
        # perf のメッセージ行はスキップ
        if not any(ch.isdigit() for ch in line.split(",")[0]):
            # 先頭フィールドが数値っぽくないものは捨てる
            continue

        parts = line.split(",")
        if len(parts) < 3:
            continue

        value_str = parts[0].strip()
        event_raw = parts[2].strip()

        try:
            value = int(value_str)
        except ValueError:
            # <not counted> など
            continue

        # "instructions:u" → "instructions" に正規化
        event_name = event_raw.split(":", 1)[0]

        counters[event_name] = value

    return counters


def print_summary(counters):
    def get(name):
        return counters.get(name, 0)

    instr = get("instructions")
    l1_loads = get("L1-dcache-loads")
    l1_miss = get("L1-dcache-load-misses")
    l2_acc = get("l2_cache_accesses_from_dc_misses")
    l2_miss = get("l2_cache_misses_from_dc_misses")
    dram_local = get("ls_refills_from_sys.ls_mabresp_lcl_dram")
    dram_remote = get("ls_refills_from_sys.ls_mabresp_rmt_dram")
    dram_total = dram_local + dram_remote

    if instr == 0:
        print("instructions is 0, cannot compute MPKI")
        return

    kinst = instr / 1000.0

    l1_mpki = l1_miss / kinst
    l2_mpki = l2_miss / kinst
    dram_mpki = dram_total / kinst

    l1_miss_rate = (l1_miss / float(l1_loads)) * 100.0 if l1_loads > 0 else 0.0
    l2_miss_rate = (l2_miss / float(l2_acc)) * 100.0 if l2_acc > 0 else 0.0

    print("=== Parsed counters ===")
    print("instructions            :", instr)
    print("L1-dcache-loads         :", l1_loads)
    print("L1-dcache-load-misses   :", l1_miss)
    print("l2_cache_accesses_from_dc_misses :", l2_acc)
    print("l2_cache_misses_from_dc_misses   :", l2_miss)
    print("DRAM fills (local+remote):", dram_total,
          "(local={}, remote={})".format(dram_local, dram_remote))
    print()

    print("=== Rates ===")
    print("L1 miss rate           : {:.2f} %".format(l1_miss_rate))
    print("L2 miss rate (on L1D misses): {:.2f} %".format(l2_miss_rate))
    print()

    print("=== MPKI (per 1K instructions) ===")
    print("L1 MPKI                : {:.3f}".format(l1_mpki))
    print("L2 MPKI                : {:.3f}".format(l2_mpki))
    print("DRAM MPKI (local+remote): {:.3f}".format(dram_mpki))


def main():
    if len(sys.argv) < 2:
        print("Usage: {} <binary> [args ...]".format(sys.argv[0]))
        sys.exit(1)

    cmd = sys.argv[1:]

    rc, out, err = run_perf(cmd)

    print("=== perf raw output (stderr) ===")
    print(err.strip())
    print()

    counters = parse_perf_csv(err)
    print_summary(counters)

    # perf の終了コードはそのまま返しておく
    sys.exit(rc)


if __name__ == "__main__":
    main()
