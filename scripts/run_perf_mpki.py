#!/usr/bin/env python3
import sys
import subprocess

# 使い方:
#   デフォルト (n05-demeter 想定; EPYC + ls_refills_from_sys.*):
#       ./scripts/run_perf_mpki.py ./benchmark 32768 536870912 524288 1 16
#
#   n07-artemis 想定 (ls_dmnd_fills_from_sys.mem_io_local を使う):
#       ./scripts/run_perf_mpki.py --node artemis ./benchmark 32768 536870912 524288 1 16
#
#   共通:
#       <binary> 以下の引数はそのまま実行ファイルに渡される
#
# 事前条件:
#   - perf がインストールされていること
#   - それぞれのノードで perf list に対応イベントが存在すること

EVENTS_DEMETER = [
    "cycles",
    "instructions",
    "L1-dcache-loads",
    "L1-dcache-load-misses",
    "l2_cache_accesses_from_dc_misses",
    "l2_cache_misses_from_dc_misses",
    "ls_refills_from_sys.ls_mabresp_lcl_dram",
    "ls_refills_from_sys.ls_mabresp_rmt_dram",
]

EVENTS_ARTEMIS = [
    "cycles",
    "instructions",
    "L1-dcache-loads",
    "L1-dcache-load-misses",
    "l2_cache_accesses_from_dc_misses",
    "l2_cache_misses_from_dc_misses",
    "ls_dmnd_fills_from_sys.mem_io_local",
    # remote 相当のイベントは無い前提なので 0 扱いにする
]

def parse_cli_args(argv):
    """
    argv から node モードと binary / args を切り出す。
    形式:
        run_perf_mpki.py [--node demeter|artemis] <binary> [binary-args...]
    """
    node = "demeter"  # デフォルト
    i = 1
    n = len(argv)

    # オプションを先頭からパース
    while i < n and argv[i].startswith("--"):
        if argv[i] == "--node":
            if i + 1 >= n:
                print("Error: --node requires an argument (demeter|artemis)", file=sys.stderr)
                sys.exit(1)
            node = argv[i + 1].lower()
            i += 2
        else:
            print("Error: unknown option {}".format(argv[i]), file=sys.stderr)
            sys.exit(1)

    if i >= n:
        print("Usage: {} [--node demeter|artemis] <binary> [binary args ...]".format(argv[0]),
              file=sys.stderr)
        sys.exit(1)

    binary = argv[i]
    args = argv[i + 1:]

    if node not in ("demeter", "artemis"):
        print("Error: --node must be 'demeter' or 'artemis'", file=sys.stderr)
        sys.exit(1)

    return node, binary, args


def run_perf(binary, args, events):
    cmd = [
        "perf", "stat",
        "-x", ",",          # CSV 形式
        "--no-big-num",     # 桁区切りなし
    ]
    for ev in events:
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


def parse_perf_stderr(stderr_text, events_of_interest):
    """
    perf stat -x, の stderr をパースして、
    {イベント名: 値(int)} の dict を返す。
    """
    counters = {}

    interest_set = set(events_of_interest)

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

        try:
            value = int(value_str)
        except ValueError:
            continue

        # event_raw には "instructions:u" のように :u が付くので落とす
        event_name = event_raw.split(":")[0]

        if event_name in interest_set:
            counters[event_name] = value

    return counters


def main():
    if len(sys.argv) < 2:
        print("Usage: {} [--node demeter|artemis] <binary> [binary args ...]"
              .format(sys.argv[0]), file=sys.stderr)
        sys.exit(1)

    node, binary, args = parse_cli_args(sys.argv)

    if node == "demeter":
        events = EVENTS_DEMETER
    else:  # "artemis"
        events = EVENTS_ARTEMIS

    result = run_perf(binary, args, events)

    # 生の stderr (perf の CSV 出力) をそのまま表示
    print("=== perf raw output (stderr) ===")
    raw = result.stderr.strip()
    if raw:
        print(raw)
    else:
        print("(no stderr output)")

    counters = parse_perf_stderr(result.stderr, events)

    # 共通イベント
    cycles = counters.get("cycles", 0)
    instr  = counters.get("instructions", 0)
    l1_loads = counters.get("L1-dcache-loads", 0)
    l1_miss  = counters.get("L1-dcache-load-misses", 0)
    l2_access_from_l1d_miss = counters.get("l2_cache_accesses_from_dc_misses", 0)
    l2_miss  = counters.get("l2_cache_misses_from_dc_misses", 0)

    # DRAM 関係
    if node == "demeter":
        dram_local  = counters.get("ls_refills_from_sys.ls_mabresp_lcl_dram", 0)
        dram_remote = counters.get("ls_refills_from_sys.ls_mabresp_rmt_dram", 0)
    else:  # artemis: local のみ
        dram_local  = counters.get("ls_dmnd_fills_from_sys.mem_io_local", 0)
        dram_remote = 0

    dram_total = dram_local + dram_remote

    print()
    print("=== Parsed counters ===")
    print("node                    : {}".format(node))
    print("cycles                 : {}".format(cycles))
    print("instructions           : {}".format(instr))
    print("L1-dcache-loads        : {}".format(l1_loads))
    print("L1-dcache-load-misses  : {}".format(l1_miss))
    print("l2_cache_accesses_from_dc_misses : {}".format(l2_access_from_l1d_miss))
    print("l2_cache_misses_from_dc_misses   : {}".format(l2_miss))
    if node == "demeter":
        print("DRAM fills (local+remote): {} (local={}, remote={})"
              .format(dram_total, dram_local, dram_remote))
    else:
        print("DRAM fills (local+remote): {} (local={}, remote assumed=0)"
              .format(dram_total, dram_local))

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

    print()
    print("=== Per-1K-instruction metrics (MPKI/PKI) ===")
    if instr == 0:
        print("instructions is 0, cannot compute MPKI/PKI")
        return

    k = instr / 1000.0

    l1_mpki   = l1_miss / k
    l2_mpki   = l2_miss / k
    dram_pki  = dram_total / k   # 「ミス」ではなく DRAM フィルなので PKI と表現

    print("L1 MPKI                : {:.3f}".format(l1_mpki))
    print("L2 MPKI                : {:.3f}".format(l2_mpki))
    print("DRAM fill PKI (local+remote): {:.3f}".format(dram_pki))


if __name__ == "__main__":
    main()
