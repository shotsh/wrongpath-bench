# wrongpath-bench Trace Surgery Context Pack (1-file)

この1ファイルを Claude Code に渡せば、背景と当面の仕様が分かるようにする。
（このファイルを `docs/WPTRACE_CONTEXT_PACK.md` として置く想定）

---

## 0. 対象リポジトリとディレクトリ想定

- 対象リポジトリ: https://github.com/shotsh/wrongpath-bench
- 置き場所案
  - このファイル: `docs/WPTRACE_CONTEXT_PACK.md`
  - ツール: `tools/`
  - 将来ログ/実験スクリプト: `scripts/`

---

## 1. 背景と狙い（会議で言われていたことの要点）

### 1.1 何をしたいか
- wrongpath-bench のマイクロベンチで、ループ末尾（post-loop, epilogue）に出てくる **B へのアクセス列**が遅い。
- アイデアとして「ループ本体の途中で、post-loop 部分を先に実行できたら（または先にプリフェッチできたら）本物の post-loop に入る頃にはキャッシュが温まり、ミスが減るのでは」を確認したい。
- いきなりマイクロアーキ実装を作るのではなく、まずは **trace surgery（トレース後処理）で一次近似の効果を見る**。

### 1.2 trace surgery でやること（一次近似）
- 元の trace は「実行された命令順」そのもの。
- trace を後処理して「post-loop の命令列（またはBアクセス列）のコピーを、ループ途中に挿入 or 置換」し、キャッシュを温める効果だけを見る。
- 注意点（会議で出ていた話）
  - ループの意味を壊さない（trip count を変える意図はない）
  - ループ本体の working set が大きすぎると、逆に追い出しが起きて損する可能性がある
  - これは本物CPUのwrong-path挙動と完全一致ではない。一次近似として割り切る

---

## 2. 重要な前提（traceフォーマットと読み方）

### 2.1 トレースは「固定長バイナリレコード列」
- ChampSim 標準の Pin トレーサ `tracer/pin` が出力する trace はテキストではなくバイナリ。
- 1命令が 1レコードで、**改行はない**。
- **固定サイズ `sizeof(input_instr)` バイト**を連続で並べたファイル。

つまり「行で読む」のではなく、
- `sizeof(input_instr)` バイトずつ `fread()` で読み取る
- N番目レコードは `offset = N * sizeof(input_instr)` にある
という世界。

### 2.2 レコードレイアウト
- tracer のコードは `inc/trace_instruction.h` の `struct input_instr` をそのまま `memcpy` して `write()` している。
- したがって「読む側」も同じ `struct input_instr` を include して `fread(&rec, sizeof(rec), 1, fp)` が基本。

重要:
- `sizeof(input_instr)` が環境やコンパイラで変わると壊れる可能性があるので、
  - まずは ChampSim の同じリポジトリ（同じヘッダ）を使ってツールをビルドする
  - `filesize % sizeof(input_instr) == 0` を必ずチェックする

---

## 3. いまやる範囲（最小の段階的ゴール）

Sho の現状スキル前提: C と Python は読める。まずは C をメインにする。

段階:
1. バイナリトレースを正しく読めていることを確認する
2. B へのアクセスが trace 中のどこに現れるか特定する
3. Bアクセス列を別位置に移動する（まずは挿入ではなく上書きコピーでOK）

この段階では「高性能」より「自分の目で正しいと確認できる」ことが重要。

---

## 4. SPEC（当面の仕様, Phase 1から3）

## 4.0 対象と前提

### 対象トレース
- ChampSim リポジトリの `tracer/pin` で生成したもの
- フォーマットは `inc/trace_instruction.h` の `struct input_instr` と同じ
- 1レコード固定長

### 圧縮（xz）
- ディスク上では `*.xz` で配布/保存されることがある
- surgeryツールはまず **解凍済み `*.trace`（生バイナリ）を前提**にする
  - 例: `xz -dk file.trace.xz` で解凍して使う
- 将来、必要なら `xz -dc` のパイプ対応は検討するが、最初はやらない

### マイクロベンチ
- 当面は `wp_bench_tuned.c` を対象
- 実行時に `printf("A=%p B=%p\n", ...)` を出す
- B の base と size はツールに引数で渡す

---

## 4.1 Phase 1: trace_inspect（読めている確認）

### 目的
- trace を `input_instr` の配列として正しく読めていることを目視確認する

### ツール
- `tools/trace_inspect.c`

### 入力
- `--trace PATH`（解凍済みのバイナリtrace）
- `--max N`（デフォルト100）
- あると便利（任意）:
  - `--start IDX`（デフォルト0）

### 出力（テキスト）
- 1レコード1行
- 出したい情報（最低限）
  - idx
  - ip
  - 非ゼロの src_mem（source_memory配列）
  - 非ゼロの dst_mem（destination_memory配列）
例:
idx=12345 ip=0x400abc src_mem=[0x7f2000001000] dst_mem=[]

### 実装要件
- `filesize % sizeof(input_instr) == 0` のチェック
- `fread()` で `sizeof(input_instr)` ずつ読む
- 表示は「確認用」なので最小限でよい

---

## 4.2 Phase 2: find_b_accesses（Bアクセス位置の特定）

### 目的
- B へのアクセスが trace 中どこにあるかを一覧化して、範囲を手で決められるようにする

### ツール
- `tools/find_b_accesses.c`

### 入力
- `--trace PATH`
- `--b-base 0x...`（printfで得たBのアドレス）
- `--b-size N`（バイト数）
- `--max-hits N`（任意）

### 判定条件
- `addr` が `b_base <= addr < b_base + b_size` なら Bアクセスとみなす
- 対象は `source_memory[]` と `destination_memory[]` の両方
- kind は load/store として区別（source側に出たらload相当, destination側に出たらstore相当）

### 出力（CSV推奨）
- `idx,kind,ip,addr,offset`
  - offset = addr - b_base（目視確認が楽）
例:
idx,kind,ip,addr,offset

123450,load,0x400c00,0x7f2200000000,0

### 期待する目視確認
- Bアクセスが「連続したチャンク」としてまとまって出る
- outer ループごとにチャンクが繰り返されていそうかが見える

---

## 4.3 Phase 3: trace_overwrite_range（簡易版, 上書きコピー）

### 目的
- `[src_begin, src_end)` のレコード列を、`[dst_begin, dst_begin + (src_end-src_begin))` に上書きコピーして改造traceを作る
- 「挿入」はやらない（最初は O(N^3) 的に爆発しがちなので避ける）
- まずは「正しくコピーできる」ことと「前後のアドレスが期待通り」だけを確認

### ツール
- `tools/trace_overwrite_range.c`

### 入力
- `--in PATH_IN`
- `--out PATH_OUT`
- `--src-begin I`
- `--src-end J`（含まない）
- `--dst-begin K`
- 任意:
  - `--dry-run`（出力せず範囲チェックだけ）

### 挙動（仕様）
- トレースを先頭から順に出力する
- idx が dst 範囲に入ったら、src 範囲のレコードを順に出力する（元のdst側レコードは捨てる）
- トレース全体の長さは変えない

### 実装要件（重要）
- まず総レコード数を数えるか、ファイルサイズから計算して範囲チェックする
- `K + (J-I) <= total_records` を満たさないならエラー終了
- src の読み方:
  - 実装を簡単にするなら「src範囲をメモリに読み込んで配列で保持」してから1パス出力でもよい
  - src範囲が大きくなったら、将来2パスやseek利用を検討
- コピー後の検証:
  - `trace_inspect` で dst周辺の数十レコードを表示して、srcと一致していることを目視する

---

## 5. 「outerごとに全部やる必要ある？」についての現実的な方針

- 理想的に「全Bチャンクを前倒し」するなら、outerごとに操作したくなるが、挿入は重い。
- 当面は次のどれかで十分:
  1) 1回だけ（1つのBチャンクだけ）をループ途中に持ってくる実験
  2) 数回だけ（例えば10回だけ）を持ってくる実験
  3) warmup/sim 区間をうまく切って「局所的IPC改善」を見る実験
- まずは「効果が出るか」を確認する一次近似が目的。全outer完全対応は後回し。

---

## 6. ChampSimでの評価の考え方（局所的に見る）

- ChampSim は通常 warmup 区間と simulation 区間で IPC/MPKI を出す。
- 「後ろは切り捨てる」運用は一般的で、局所的に見たいなら
  - トレース生成時の `-s`（skip）と `-t`（trace count）
  - ChampSimの warmup_instructions / simulation_instructions
  を組み合わせて、見たい範囲にフォーカスする。

当面の実務方針:
- surgeryで作ったtraceは長さを変えない（上書き）
- ChampSim 実行では warmup と sim を短めから試し、効果が見えるかだけ見る

---

## 7. 用語ミニ辞書

- record: traceの1命令分の固定長バイナリ構造体
- src_mem / dst_mem: `source_memory[]` / `destination_memory[]`
- B chunk: outer 1回でアクセスするBの連続範囲（例: 1024要素分など）
- overwrite: 置換。長さを変えずに別レコード列で上書き
- insert: 挿入。長さが増え、後ろがシフトする（当面はやらない）
- trace surgery: traceの後処理で命令列を改造すること（一次近似評価）

---

## 8. Claude Code への指示（最初にやること）

### 8.1 Claudeに読ませるもの
このファイル1つだけでよい（このファイルが背景+SPECの役割）。

### 8.2 最初の指示テンプレ（コピペ用）
You are working in repo: wrongpath-bench.

Read: docs/WPTRACE_CONTEXT_PACK.md.



Task 1:

Implement tools/trace_inspect.c (Phase 1).

Constraints:

C only (no C++)

Include ChampSim’s inc/trace_instruction.h and read input_instr records

Validate filesize % sizeof(input_instr) == 0

CLI: –trace PATH, optional –max N, optional –start IDX

Output one record per line: idx, ip, nonzero source_memory[], nonzero destination_memory[]

Keep code minimal and readable, add usage() and error handling



After implementation:

Provide build command example and a tiny example of expected output format.

### 8.3 次の指示テンプレ（Phase 2）
Task 2:

Implement tools/find_b_accesses.c (Phase 2).

CLI: –trace PATH –b-base 0x… –b-size N [–max-hits N]

Output CSV: idx,kind,ip,addr,offset

Same constraints: C only, validate record size alignment.

### 8.4 次の指示テンプレ（Phase 3）
Task 3:

Implement tools/trace_overwrite_range.c (Phase 3).

CLI: –in IN –out OUT –src-begin I –src-end J –dst-begin K [–dry-run]

Behavior: overwrite dst range with src records, keep total length unchanged.

Validate ranges and fail fast with clear errors.

---

## 9. 非ゴール（今はやらない）
- ループ自動検出
- 挿入モード（長さ変更）
- xzをツール側で直接読む対応（まずは解凍して使う）
- 本物CPUのwrong-path厳密モデル化