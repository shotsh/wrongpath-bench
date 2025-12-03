#!/usr/bin/env python3
import argparse
import csv
import datetime
import subprocess
import re
from pathlib import Path

# 作業ディレクトリ（シンボリックリンク経由でも OK）
ROOT = Path.cwd()
CONFIG_PATH = ROOT / "configs" / "cases.csv"
RESULT_ROOT = ROOT / "results"
BENCH = ROOT / "benchmark"
RUN_PERF = ROOT / "scripts" / "run_perf_mpki.py"

RE_IPC      = re.compile(r"^IPC\s*:\s*([0-9.]+)")
RE_L1_MPKI  = re.compile(r"^L1 MPKI\s*:\s*([0-9.]+)")
RE_L2_MPKI  = re.compile(r"^L2 MPKI\s*:\s*([0-9.]+)")
RE_DRAM_PKI = re.compile(r"^DRAM fill PKI .*:\s*([0-9.]+)")

def load_cases():
    cases = {}
    with open(CONFIG_PATH, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            cases[row["case_id"]] = row
    return cases

def build_argv_list(case):
    """
    cases.csv の 1 行から、benchmark に渡す引数リストを作る。
      A_bytes B_bytes chunk_bytes [access_mode] [stride_elems] [outer_scale]
    """
    args = []

    # 必須3つ
    for key in ["A_bytes", "B_bytes", "chunk_bytes"]:
        raw = (case.get(key) or "").strip()
        if not raw:
            raise ValueError(f"{key} is empty for case_id={case.get('case_id')}")
        val = int(raw, 0)  # 10進でも 0x8000 でも OK
        args.append(str(val))

    # optional3つ（左から順に埋める前提）
    opt_keys = ["access_mode", "stride_elems", "outer_scale"]
    opt_raws = [(case.get(k) or "").strip() for k in opt_keys]

    # 途中だけ空欄があるとややこしいのでチェック
    seen_empty = False
    for k, raw in zip(opt_keys, opt_raws):
        if raw == "":
            seen_empty = True
        else:
            if seen_empty:
                raise ValueError(
                    "case_id={cid}: '{k}' is set but a previous optional field is empty"
                    .format(cid=case.get("case_id"), k=k)
                )

    for raw in opt_raws:
        if raw != "":
            val = int(raw, 0)
            args.append(str(val))
        else:
            break

    return args

def run_one_case(case, outdir):
    """1ケース分実行して、生ログとサマリ行を返す。"""
    case_id = case["case_id"]
    description = (case.get("description") or "").strip()  # あってもなくてもOK

    argv = build_argv_list(case)

    A_bytes     = int(argv[0])
    B_bytes     = int(argv[1])
    chunk_bytes = int(argv[2])

    access_mode  = int(argv[3]) if len(argv) >= 4 else None
    stride_elems = int(argv[4]) if len(argv) >= 5 else None
    outer_scale  = int(argv[5]) if len(argv) >= 6 else None

    outdir.mkdir(parents=True, exist_ok=True)

    # ★ ここでタイムスタンプ付きファイル名を作る
    ts = datetime.datetime.now().strftime("%y%m%d%H%M%S")  # yymmddHHMMSS
    logfile = outdir / "perf_{cid}_{ts}.txt".format(cid=case_id, ts=ts)

    # ヘッダブロック（ターミナル & ログファイル共通）
    header_lines = [
        "== Running case {cid} ==".format(cid=case_id),
        "  log_file    : {name}".format(name=logfile.name),
        "  A_bytes      : {v}".format(v=A_bytes),
        "  B_bytes      : {v}".format(v=B_bytes),
        "  chunk_bytes  : {v}".format(v=chunk_bytes),
        "  access_mode  : {v}".format(v=access_mode),
        "  stride_elems : {v}".format(v=stride_elems),
        "  outer_scale  : {v}".format(v=outer_scale),
    ]
    if description:
        header_lines.append("  description  : {d}".format(d=description))

    header_text = "\n".join(header_lines) + "\n"

    # ターミナルに表示
    print(header_text)

    cmd = ["python3", str(RUN_PERF), str(BENCH)] + [str(x) for x in argv]

    # Python 3.6 対応の subprocess
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    stdout, stderr = proc.communicate()

    # ターミナルにも perf の出力をそのまま表示
    print("=== STDOUT (run_perf_mpki) ===")
    print(stdout, end="")
    print("\n=== STDERR (run_perf_mpki) ===")
    print(stderr, end="")
    print()

    if proc.returncode != 0:
        raise RuntimeError(
            "run_perf failed for case {cid} with code {code}\ncmd: {cmd}\nstderr:\n{stderr}".format(
                cid=case_id, code=proc.returncode, cmd=" ".join(cmd), stderr=stderr
            )
        )

    # 生ログをファイルに保存（ヘッダ → CMD → STDOUT → STDERR の順）
    with open(logfile, "w") as f:
        f.write(header_text)
        f.write("\n=== CMD ===\n")
        f.write(" ".join(cmd) + "\n\n")
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
        print("Warning: failed to parse some metrics for {cid}".format(cid=case_id))
    else:
        print(
            "  ==> Parsed summary: IPC={:.3f}, L1_MPKI={:.3f}, "
            "L2_MPKI={:.3f}, DRAM_PKI={:.3f}".format(
                ipc, l1_mpki, l2_mpki, dram_pki
            )
        )
    print()

    return {
        "case_id": case_id,
        "A_bytes": A_bytes,
        "B_bytes": B_bytes,
        "chunk_bytes": chunk_bytes,
        "access_mode": access_mode,
        "stride_elems": stride_elems,
        "outer_scale": outer_scale,
        "IPC": ipc,
        "L1_MPKI": l1_mpki,
        "L2_MPKI": l2_mpki,
        "DRAM_PKI": dram_pki,
        "description": description,  # サマリでは最後の列
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

    date_str = datetime.datetime.now().strftime("%Y%m%d")
    outdir = RESULT_ROOT / date_str
    outdir.mkdir(parents=True, exist_ok=True)

    summary_path = outdir / "summary.csv"
    summary_rows = []

    for cid in target_ids:
        if cid not in cases:
            print("Error: case_id '{cid}' not found in {cfg}".format(
                cid=cid, cfg=CONFIG_PATH
            ))
            continue
        row = run_one_case(cases[cid], outdir)
        summary_rows.append(row)

    if summary_rows:
        write_header = not summary_path.exists()
        fieldnames = [
            "case_id",
            "A_bytes", "B_bytes", "chunk_bytes",
            "access_mode", "stride_elems", "outer_scale",
            "IPC", "L1_MPKI", "L2_MPKI", "DRAM_PKI",
            "description",  # 最後に description
        ]
        with open(summary_path, "a", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            if write_header:
                writer.writeheader()
            for r in summary_rows:
                writer.writerow(r)

        print("Wrote summary to {path}".format(path=summary_path))

if __name__ == "__main__":
    main()
