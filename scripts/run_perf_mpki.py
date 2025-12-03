#!/usr/bin/env python3
import sys
import subprocess

# このスクリプトの使い方:
#   ./scripts/run_perf_mpki.py <binary> [binary args...]
#
# 例:
#   ./scripts/run_perf_mpki.py ./benchmark 32768 536870912 524288 1 8
#
# 前提:
#   - perf がインストール済み
#   - イベント名は perf list で存在すること

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


def run_perf(binary, args):
    cmd = [
        "perf", "stat",
        "-x", ",",          # CSV 形式
        "--no-big-num",     # 桁区切りなし
    ]
    for ev in EVENTS:
        cmd += ["-e", ev]
    cmd.append(binary)
    cmd += args

    # Python 3.6 対応: text= ではなく universal_newlines=
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    return result


def parse_perf_stderr(stderr_text):
    """
    perf stat -x, の stderr をパースして、
    {イベント名: 値(int)} の dict を返す。
    """
    counters = {}

    for line in stderr_text.splitlines():
        line = line.strip()
        if not line:
            continue

        # 形式: value,unit,event,.... (カンマ区切り)
        parts = line.split(",")
        if len(parts) < 3:
            continue

        value_str = parts[0].strip()
        event_raw = parts[2].strip()

        # "<not counted>" や "<not supported>" の行などはスキップ
        if value_str in ("<not counted>", "<not supported>"):
            continue

        # 値パース
        try:
            value = int(value_str)
        except ValueError:
            continue

        # event_raw には "instructions:u" のように :u が付くので落とす
        event_name = event_raw.split(":")[0]

        # 我々が指定した名前だけを拾う
        if event_name in EVENTS:
            counters[event_name] = value

    return counters


def main():
    if len(sys.argv) < 2:
        print("Usage: {} <binary> [binary args ...]".format(sys.argv[0]),
              file=sys.stderr)
        sys.exit(1)

    binary = sys.argv[1]
    args = sys.argv[2:]

    result = run_perf(binary, args)

    # 生の stderr (perf の CSV 出力) をそのまま表示
    print("=== perf raw output (stderr) ===")
    raw = result.stderr.strip()
    if raw:
        print(raw)
    else:
        print("(no stderr output)")

    counters = parse_perf_stderr(result.stderr)

    cycles = counters.get("cycles", 0)
    instr = counters.get("instructions", 0)
    l1_loads = counters.get("L1-dcache-loads", 0)
    l1_miss  = counters.get("L1-dcache-load-misses", 0)
    l2_access_from_l1d_miss = counters.get("l2_cache_accesses_from_dc_misses", 0)
    l2_miss  = counters.get("l2_cache_misses_from_dc_misses", 0)
    dram_local  = counters.get("ls_refills_from_sys.ls_mabresp_lcl_dram", 0)
    dram_remote = counters.get("ls_refills_from_sys.ls_mabresp_rmt_dram", 0)
    dram_total  = dram_local + dram_remote

    print()
    print("=== Parsed counters ===")
    print("cycles                 : {}".format(cycles))
    print("instructions           : {}".format(instr))
    print("L1-dcache-loads        : {}".format(l1_loads))
    print("L1-dcache-load-misses  : {}".format(l1_miss))
    print("l2_cache_accesses_from_dc_misses : {}".format(l2_access_from_l1d_miss))
    print("l2_cache_misses_from_dc_misses   : {}".format(l2_miss))
    print("DRAM fills (local+remote): {} (local={}, remote={})"
          .format(dram_total, dram_local, dram_remote))

    # レート / IPC 計算
    print()
    print("=== Rates / IPC ===")
    if l1_loads > 0:
        l1_miss_rate = 100.0 * l1_miss / l1_loads
        print("L1 miss rate           : {:.2f} %".format(l1_miss_rate))
    else:
        print("L1 miss rate           : N/A (L1-dcache-loads == 0)")

    if l2_access_from_l1d_miss > 0:
        l2_miss_rate = 100.0 * l2_miss / l2_access_from_l1d_miss
        print("L2 miss rate (on L1D misses): {:.2f} %".format(l2_miss_rate))
    else:
        print("L2 miss rate (on L1D misses): N/A (L2 accesses from L1D misses == 0)")

    if cycles > 0:
        ipc = float(instr) / float(cycles)
        print("IPC                    : {:.3f}".format(ipc))
    else:
        print("IPC                    : N/A (cycles == 0)")

    # MPKI / PKI 計算
    print()
    print("=== Per-1K-instruction metrics (MPKI/PKI) ===")
    if instr == 0:
        print("instructions is 0, cannot compute MPKI/PKI")
        return

    k = instr / 1000.0

    l1_mpki = l1_miss / k
    l2_mpki = l2_miss / k
    dram_pki = dram_total / k   # 「ミス」ではなく DRAM フィルなので PKI と表現

    print("L1 MPKI                : {:.3f}".format(l1_mpki))
    print("L2 MPKI                : {:.3f}".format(l2_mpki))
    print("DRAM fill PKI (local+remote): {:.3f}".format(dram_pki))


if __name__ == "__main__":
    main()
