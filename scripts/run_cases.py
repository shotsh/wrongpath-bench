#!/usr/bin/env python3
import argparse
import csv
import datetime
import os
import subprocess
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG_PATH = ROOT / "configs" / "cases.csv"
RESULT_ROOT = ROOT / "results"
BENCH = ROOT / "benchmark"
RUN_PERF = ROOT / "scripts" / "run_perf_mpki.py"

# perf の出力からサマリを抜くための簡単な正規表現
RE_IPC = re.compile(r"^IPC\s*:\s*([0-9.]+)")
RE_L1_MPKI = re.compile(r"^L1 MPKI\s*:\s*([0-9.]+)")
RE_L2_MPKI = re.compile(r"^L2 MPKI\s*:\s*([0-9.]+)")
RE_DRAM_PKI = re.compile(r"^DRAM fill PKI .*:\s*([0-9.]+)")

def load_cases():
    cases = {}
    with open(CONFIG_PATH, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            cases[row["case_id"]] = row
    return cases

def run_one_case(case, outdir):
    """1ケース分実行して、生ログとサマリ行を返す。"""
    case_id = case["case_id"]
    desc = case.get("desc", "")

    A_KiB = int(case["A_KiB"])
    B_MiB = int(case["B_MiB"])
    chunk_KiB = int(case["chunk_KiB"])
    B_stride = int(case["B_stride_elems"])
    outer_scale = int(case["outer_scale"])
    kernel_reps = int(case["kernel_reps"])

    A_BYTES = A_KiB * 1024
    B_BYTES = B_MiB * 1024 * 1024
    CHUNK_BYTES = chunk_KiB * 1024

    outdir.mkdir(parents=True, exist_ok=True)
    logfile = outdir / f"perf_{case_id}.txt"

    print(f"== Running case {case_id} ==")
    print(f"  desc            : {desc}")
    print(f"  A_bytes         : {A_BYTES} ({A_KiB} KiB)")
    print(f"  B_bytes         : {B_BYTES} ({B_MiB} MiB)")
    print(f"  chunk_bytes     : {CHUNK_BYTES} ({chunk_KiB} KiB)")
    print(f"  B_stride_elems  : {B_stride}")
    print(f"  outer_scale     : {outer_scale}")
    print(f"  kernel_reps     : {kernel_reps}")
    print()

    cmd = [
        "python3", str(RUN_PERF),
        str(BENCH),
        str(A_BYTES),
        str(B_BYTES),
        str(CHUNK_BYTES),
        str(outer_scale),
        str(B_stride),
        str(kernel_reps),
    ]

    # 実行して stdout をファイルにも保存
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    stdout = proc.stdout
    stderr = proc.stderr

    with open(logfile, "w") as f:
        f.write("=== STDOUT ===\n")
        f.write(stdout)
        f.write("\n\n=== STDERR ===\n")
        f.write(stderr)

    # stdout から IPC / MPKI / DRAM PKI をパース
    ipc = l1_mpki = l2_mpki = dram_pki = None
    for line in stdout.splitlines():
        m = RE_IPC.search(line)
        if m:
            ipc = float(m.group(1))
            continue
        m = RE_L1_MPKI.search(line)
        if m:
            l1_mpki = float(m.group(1))
            continue
        m = RE_L2_MPKI.search(line)
        if m:
            l2_mpki = float(m.group(1))
            continue
        m = RE_DRAM_PKI.search(line)
        if m:
            dram_pki = float(m.group(1))
            continue

    if ipc is None or l1_mpki is None or l2_mpki is None or dram_pki is None:
        print(f"Warning: failed to parse some metrics for {case_id}")

    # サマリ行の dict を返す
    return {
        "case_id": case_id,
        "desc": desc,
        "A_bytes": A_BYTES,
        "B_bytes": B_BYTES,
        "chunk_bytes": CHUNK_BYTES,
        "B_stride_elems": B_stride,
        "outer_scale": outer_scale,
        "kernel_reps": kernel_reps,
        "IPC": ipc,
        "L1_MPKI": l1_mpki,
        "L2_MPKI": l2_mpki,
        "DRAM_PKI": dram_pki,
    }

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--case",
        nargs="*",
        help="run only these case_id(s). omit and use --all for all cases.",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="run all cases in configs/cases.csv",
    )
    args = parser.parse_args()

    cases = load_cases()

    if args.all:
        target_ids = list(cases.keys())
    elif args.case:
        target_ids = args.case
    else:
        parser.error("either --all or --case must be specified")

    # 日付ごとにフォルダを分ける
    date_str = datetime.datetime.now().strftime("%Y%m%d")
    outdir = RESULT_ROOT / date_str
    outdir.mkdir(parents=True, exist_ok=True)

    summary_path = outdir / "summary.csv"
    summary_rows = []

    for cid in target_ids:
        if cid not in cases:
            print(f"Error: case_id '{cid}' not found in {CONFIG_PATH}")
            continue
        row = run_one_case(cases[cid], outdir)
        summary_rows.append(row)

    if summary_rows:
        # CSV に追記。初回はヘッダ付きで書く
        write_header = not summary_path.exists()
        fieldnames = [
            "case_id", "desc",
            "A_bytes", "B_bytes", "chunk_bytes",
            "B_stride_elems", "outer_scale", "kernel_reps",
            "IPC", "L1_MPKI", "L2_MPKI", "DRAM_PKI",
        ]
        with open(summary_path, "a", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            if write_header:
                writer.writeheader()
            for r in summary_rows:
                writer.writerow(r)

        print()
        print(f"Wrote summary to {summary_path}")

if __name__ == "__main__":
    main()
