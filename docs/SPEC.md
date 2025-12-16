# wrongpath-bench trace surgery SPEC

## 改訂履歴

| 日付 | バージョン | 変更内容 |
|------|-----------|----------|
| 2025-12-16 | 1.4 | Phase 4 (trace_insert_all_iters) 仕様追加: 全イテレーションへの一括挿入 |
| 2025-12-16 | 1.3 | Phase 3.6 (trace_insert_b_at_a) 仕様追加: Aの位置とB挿入量をパラメータで指定 |
| 2025-12-16 | 1.2 | Phase 3.5 (trace_insert_range) 仕様追加 |
| 2025-12-15 | 1.1 | Phase 3 (trace_overwrite_range) 仕様追加、配列A特定セクション追加 |
| 2025-12-14 | 1.0 | 初版: Phase 1-2 仕様 |

---

## 0. 対象リポジトリとファイル配置

* 対象リポジトリ: [https://github.com/shotsh/wrongpath-bench](https://github.com/shotsh/wrongpath-bench)
* この SPEC は `wrongpath-bench/docs/wpsurgery_spec.md` として置く
* ツール類は `wrongpath-bench/tools/` 以下に置く

---

## 1. このプロジェクトでやりたいこと

`wrongpath-bench` で作ったマイクロベンチマークに対して、ChampSim 標準の Pin トレーサで生成した **バイナリ形式トレース**をいじるツールを作る。

最初の目的は、以下の 3 段階をきちんとこなせるようになること。

1. バイナリトレースを「正しく読めている」ことを確認する
2. 配列 B へのメモリアクセスが、トレース中のどこに出てくるかを特定できるようにする
3. 狙った範囲のトレース（今回は B へのアクセス列）を、別の位置に「移動させる」

   * まずは「間に挿入」ではなく、単純な **上書きコピー** でよい（道具の動作確認用）

いきなり難しいことはやらず、

* 「読む」フェーズと
* 「書き換える」フェーズ

を分けて、段階的に進める。

言語は、Sho が慣れている **Cをメインターゲット**とする。
（内部実装で C++ を使うのはアリだが、必須ではない）

---

## 2. 対象と前提

### 2.1 対象トレース

* ChampSim リポジトリの `tracer/pin` 以下にある標準トレーサで生成したもの
* フォーマットは `inc/trace_instruction.h` の `struct input_instr` と同じバイナリレイアウト
* 1 レコードは固定長（`sizeof(input_instr)` バイト）
* **重要: ツール側が想定する `input_instr` の定義は、トレース生成に使った ChampSim と同一コミットの `inc/trace_instruction.h` を参照する**

  * コンパイラやABI差で `sizeof(input_instr)` が変わり得るため

### 2.2 圧縮について

* ディスク上では `*.xz` で圧縮しておいて良い
* **編集や解析をするときは、いったん `xz -d` などで解凍して `*.trace`（生バイナリ）を扱う**
* ChampSim 本体は `*.xz` も読めるが、trace surgery ツールは当面 **生バイナリ (`*.trace`) 前提**とする

### 2.3 マイクロベンチ

* 当面は `wp_bench_tuned.c` のみを対象にする
* ベンチマーク側で `printf("A=%p B=%p\n", (void*)A, (void*)B);` のように出力しておく
* **B のベースアドレスは「トレース生成と同一実行」で出力した値を使う**

  * ASLR により別実行のアドレスは一致しない可能性が高い
* B のサイズは `ARRAY_B_SIZE * sizeof(double)` を基本とする

---

## 3. フェーズ別の仕様

## 3.1 フェーズ1: トレースを読めていることの確認

**目的**
バイナリトレースが本当に `input_instr` の配列として読めているかを、自分の目で確認できるようにする。

**このフェーズのゴール**

* `trace_inspect` の出力を目視して
  「A のアクセス」「B のアクセス」がそれっぽいアドレスに見えていると自信を持てること

**ツール案**
`tools/trace_inspect.c`

### 入力

* `--trace PATH`
  解凍済みのバイナリトレースファイルへのパス
* オプション

  * `--max N`
    表示するレコード数（デフォルト 100）
  * （将来拡張）`--begin IDX --count N`
    特定範囲だけ見るため

### 出力（テキスト）

標準出力に、1 レコード 1 行程度で人間が読める形のダンプを出す。

最低限欲しい情報:

* レコード番号（0 始まり）
* `ip`
* `source_memory[]` / `destination_memory[]` のうち、ゼロでないものだけ列挙

例:

```text
idx=12345 ip=0x400abc src_mem=[0x7f2000001000] dst_mem=[]
idx=12346 ip=0x400ac0 src_mem=[0x7f2000001008] dst_mem=[]
...
```

### 実装上のポイント

* `sizeof(input_instr)` バイトずつ順に読み取る

  * Cなら `struct input_instr rec; fread(&rec, sizeof(rec), 1, fp);`
* **起動時に sanity check を行う**

  * `filesize % sizeof(input_instr) == 0` でないならエラー（レイアウト不一致や破損の可能性）
* ここでは「全フィールドを完璧に表示する」ことは目的ではなく、
  **メモリアクセスのアドレスがそれっぽく見えることの確認**が目的

---

## 3.2 フェーズ2: 配列 B へのアクセス位置の特定

**目的**
「配列 B へのロード/ストアがトレースのどこに現れているか」を一覧できるようにする。

**このフェーズのゴール**

* `find_b_accesses` の出力から

  * B へのアクセスが「この辺りのレコードに固まっている」
  * outer ループごとに連続したチャンクになっていそう
    という感覚を掴めること

**ツール案**
`tools/find_b_accesses.c`

### 入力

* `--trace PATH`
  解凍済みのバイナリトレース
* `--b-base 0x...`
  トレース生成と同一実行で得た `B=%p` を 16 進で渡す
* `--b-size N`
  `ARRAY_B_SIZE * sizeof(double)`（バイト数）
* オプション

  * `--max-hits N`
    ヒット件数上限（デフォルト無制限でも良いが、実用上は 1e6 など）

### 判定条件

* 1 レコード中の `source_memory[]` または `destination_memory[]` のアドレス `addr` について

  * `b_base <= addr < b_base + b_size` なら「B へのアクセス」

### 出力（CSV推奨）

最低限:

* レコード番号 `idx`
* 種別 `kind`（load/store）
* `ip`
* `addr`
* **`offset = addr - b_base`**（目視検証用）

例:

```text
idx,kind,ip,addr,offset
123450,load,0x400c00,0x7f2200000000,0x0
123451,load,0x400c00,0x7f2200000008,0x8
...
```

### 目視で確認したいこと

* `offset` が `+8` で連続する（double配列の連続アクセスが見える）
* outer ごとに「1024要素チャンク」っぽいまとまりが周期的に出る

---

## 3.2.1 配列 A へのアクセス位置の特定

**目的**
配列 B の移動先として、配列 A のアクセス位置を特定する必要がある。
カーネル構造上、「A sweep → B chunk」の順でアクセスが発生するため、
B チャンクを A の直後（または途中）に挿入するには A の位置を知る必要がある。

**このフェーズのゴール**

* 配列 A へのアクセスがトレース中のどこに現れるかを特定できること
* A と B のアクセスパターンの境界を把握できること

### カーネル構造の理解

`benchmark.c` の `run_kernel` は以下の構造:

```c
for (size_t outer = 0; outer < outer_iters; outer++) {
    // 1) 配列A sweep (L1キャッシュを乱す)
    for (size_t i = 0; i < A_elems; i++) {
        sum += A[i];
    }

    // 2) 配列B chunk アクセス
    for (size_t j = 0; j < elems_per_iter; j++) {
        sum += B[base + j * stride_elems];
    }
}
```

### A の IP アドレス特定方法

A へのアクセスは B とは異なる IP で行われる。
`objdump` でディスアセンブリを確認:

```bash
objdump -d benchmark_trace | grep -A100 '<main>'
```

コンパイラ最適化（ループアンロール）により、A アクセスは複数の IP で行われることが多い。

### trace_inspect を使った確認

```bash
# A アクセスの IP をフィルタ
./trace_inspect --trace <PATH> --max 500000 \
    | grep -E 'ip=0x400880|ip=0x40088c'
```

### A のアドレス範囲特定

```bash
# ユニークアドレスを抽出
./trace_inspect --trace <PATH> --max 10000000 \
    | grep -E 'ip=0x400880|ip=0x40088c' \
    | awk -F'src_mem=\\[' '{print $2}' | cut -d']' -f1 \
    | sort -u > /tmp/a_addrs.txt

# 範囲確認
head -1 /tmp/a_addrs.txt  # 最小アドレス
tail -1 /tmp/a_addrs.txt  # 最大アドレス
wc -l /tmp/a_addrs.txt    # ユニークアドレス数（= A_elems であるべき）
```

### A と B のアクセス比較

| 項目 | 配列A | 配列B |
|------|-------|-------|
| IP | 複数（ループアンロール） | 単一（通常） |
| アドレス範囲 | 低位（ヒープ先頭付近） | 高位（mmap領域、下位32bitで記録） |
| サイズ | `A_bytes` (例: 64KB) | `B_bytes * stride` (例: 1GB) |
| ストライド | 8 bytes (連続) | `stride_elems * 8` bytes |
| 1 outer iter | `A_elems` アクセス | `elems_per_iter` アクセス |

### 注意点

* **A のアドレスは下位32ビットでも元のアドレスと一致することが多い**
  （ヒープの低いアドレスに配置されるため）
* **B のアドレスは mmap で高位に配置されるため、下位32ビットのみが記録される**

---

## 3.3 フェーズ3: トレース書き換え（簡易版、上書きコピー）

**目的**
「B にアクセスしている連続したチャンク」を、トレース中の別の位置にコピーして上書きし、
ChampSim に食わせられる改造トレースを作る。

**注意（位置づけ）**
上書きコピーは「ツールが狙い通り書き換えられる」ことの確認用。
会議の意図に近い「途中に差し込んで先読みする」評価は、将来の挿入モードで行う。

**このフェーズのゴール**

* 指定した `[src_begin, src_end)` のレコード列が
  `[dst_begin, dst_begin + (src_end - src_begin))` に
  期待通りコピーされていることを `trace_inspect` などで確認できること

**初回の想定シナリオ**

* 「1個の outer iteration に対応する B チャンク」をコピー元に選び
* 「別の outer iteration の近く」を書き換え先に選ぶ
* これで「ある反復の途中に他の反復の B アクセス列を持ってくる」形を試す（まずは動作確認）

**ツール案**
`tools/trace_overwrite_range.c`

### 入力

* `--in PATH_IN`
  元のバイナリトレース
* `--out PATH_OUT`
  書き換え後のバイナリトレース
* コピー元範囲

  * `--src-begin I`（含む）
  * `--src-end J`（含まない）
* 書き換え先

  * `--dst-begin K`
  * 制約: `K + (J - I) <= 総レコード数`（呼び出し側で保証）

### 挙動

* 元トレースを先頭から順に読みながら

  * `idx` が書き換え範囲内なら、コピー元を順に出力
  * それ以外は元レコードをそのまま出力
* ブランチ情報・レジスタ情報なども含め、レコードは丸ごとコピーする
* トレース全体の長さは変えない

---

## 3.4 フェーズ3.5: トレース書き換え（挿入モード）

**目的**
「B にアクセスしている連続したチャンク」を、トレース中の指定位置に **挿入** する。
上書きモードと異なり、元のレコードはすべて保持され、トレース長が増加する。

**このフェーズのゴール**

* 指定した `[src_begin, src_end)` のレコード列が
  `insert_at` の位置に挿入されていることを確認できること
* 元のレコードがすべて保持されていることを確認できること

**ツール**
`tools/trace_insert_range.c`

### 入力

* `--in PATH_IN`
  元のバイナリトレース
* `--out PATH_OUT`
  挿入後のバイナリトレース
* コピー元範囲
  * `--src-begin I`（含む）
  * `--src-end J`（含まない）
* 挿入位置
  * `--insert-at K`（このインデックスの直前に挿入）
* オプション
  * `--dry-run`（出力せず範囲チェックと出力マッピング表示のみ）

### 挙動

1. src範囲 `[src_begin, src_end)` をメモリに読み込む
2. 入力を先頭から順に読みながら:
   * `idx < insert_at`: そのまま出力
   * `idx == insert_at`: src範囲を出力（挿入）、その後元レコードも出力
   * `idx > insert_at`: そのまま出力
3. 出力トレース長 = 入力トレース長 + (src_end - src_begin)

### 出力インデックスのマッピング

```
入力: [0, ..., insert_at-1, insert_at, ..., total-1]
出力: [0, ..., insert_at-1, INSERTED..., insert_at+len, ..., total+len-1]
                            ^^^^^^^^^^
                            挿入された部分
```

### 典型的なユースケース

**Bチャンクの先読み効果を検証（挿入モード）:**

1. `find_b_accesses` で最初のBチャンクの範囲を特定
2. 配列Aアクセスの途中を `insert_at` として指定
3. Bチャンクをその位置に挿入
4. ChampSim で元トレースと改造トレースの IPC を比較

```bash
# 例: Aの真ん中 (idx=336480) にBチャンクを挿入
./trace_insert_range \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out ../wp_inserted.trace \
    --src-begin 350820 --src-end 371295 \
    --insert-at 336480
```

---

## 3.6 フェーズ3.6: 簡易挿入ツール (trace_insert_b_at_a)

**目的**
Phase 3.5 の `trace_insert_range` は汎用的だが、パラメータ（src-begin, src-end, insert-at）を
毎回計算する必要がある。パラメータスイープ実験を効率的に行うため、より直感的なパラメータで
挿入を指定できるツールを提供する。

**設計思想**

1. **「Aのどこに」**: Aスイープ内の挿入位置を比率（0.0〜1.0）で指定
2. **「Bのどれくらい」**: Bチャンクの先頭から何レコード（または比率）を挿入するか

これにより、以下のようなパラメータスイープが容易になる:
- A挿入位置: 0.0, 0.25, 0.5, 0.75, 1.0
- B挿入量: 0.25, 0.5, 0.75, 1.0

**ツール**
`tools/trace_insert_b_at_a.c`

### 入力

* `--in PATH_IN`
  元のバイナリトレース
* `--out PATH_OUT`
  挿入後のバイナリトレース
* `--a-begin IDX`
  配列Aスイープの開始インデックス（含む）
* `--a-end IDX`
  配列Aスイープの終了インデックス（含まない）
* `--b-begin IDX`
  配列Bチャンクの開始インデックス（含む）
* `--b-end IDX`
  配列Bチャンクの終了インデックス（含まない）
* `--a-pos RATIO`
  Aスイープ内の挿入位置（0.0〜1.0）
  - 0.0: Aの先頭に挿入
  - 0.5: Aの真ん中に挿入
  - 1.0: Aの最後（Bの直前）に挿入
* `--b-ratio RATIO`
  Bチャンクのうち挿入する割合（0.0〜1.0）
  - 0.5: Bチャンクの前半を挿入
  - 1.0: Bチャンク全体を挿入
* オプション
  * `--dry-run`
    出力せず計算結果のみ表示

### 内部計算

```
insert_at = a_begin + (int)((a_end - a_begin) * a_pos)
b_insert_len = (int)((b_end - b_begin) * b_ratio)
src_begin = b_begin
src_end = b_begin + b_insert_len
```

### 典型的な使用例

```bash
# A と B の範囲を find_b_accesses と trace_inspect で特定済みとする
# A: idx=322141 ~ 350820, B: idx=350820 ~ 371296

# ケース1: Aの真ん中にBチャンク全体を挿入
./trace_insert_b_at_a \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out /tmp/test1.trace \
    --a-begin 322141 --a-end 350820 \
    --b-begin 350820 --b-end 371296 \
    --a-pos 0.5 --b-ratio 1.0

# ケース2: Aの先頭にBチャンクの前半を挿入
./trace_insert_b_at_a \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out /tmp/test2.trace \
    --a-begin 322141 --a-end 350820 \
    --b-begin 350820 --b-end 371296 \
    --a-pos 0.0 --b-ratio 0.5

# パラメータスイープ例
for a_pos in 0.0 0.25 0.5 0.75 1.0; do
    for b_ratio in 0.25 0.5 0.75 1.0; do
        ./trace_insert_b_at_a \
            --in ../wp_trace \
            --out /tmp/sweep_a${a_pos}_b${b_ratio}.trace \
            --a-begin 322141 --a-end 350820 \
            --b-begin 350820 --b-end 371296 \
            --a-pos $a_pos --b-ratio $b_ratio
    done
done
```

### 出力情報

ツールは実行時に以下の情報を標準エラーに出力する:

```
# Input: ../wp_A64KB_B64MB_chunk32KB_stride16_os2
# A range: [322141, 350820) (28679 records)
# B range: [350820, 371296) (20476 records)
# Parameters: a_pos=0.5, b_ratio=1.0
# Calculated:
#   insert_at = 336480 (A midpoint)
#   B insert: [350820, 371296) (20476 records)
# Output records: 201707970 + 20476 = 201728446
```

### 制約事項

* 単一のAスイープ、単一のBチャンクに対する単一挿入のみ対応
* 複数 outer iteration への一括挿入は Phase 4 で対応

---

## 4. フェーズ4: 全イテレーション一括挿入 (trace_insert_all_iters)

**目的**
Phase 3.6 の単一挿入を、全 outer iteration に対して一括で適用する。
各イテレーションで同じパラメータ (a_pos, b_ratio) を使用し、
「全ループで同じタイミング・同じ量の先読みを行う」効果を検証する。

**設計思想**

1. **パラメータの再利用**: Phase 3.6 と同じ `a_pos`, `b_ratio` を使用
2. **構造の規則性を活用**: マイクロベンチは A/B の長さが全イテレーションで固定
3. **1パス処理**: 挿入位置を事前計算し、トレースを1回読みながら全挿入を実行

**ツール**
`tools/trace_insert_all_iters.c`

### 入力

* `--in PATH_IN`
  元のバイナリトレース
* `--out PATH_OUT`
  挿入後のバイナリトレース
* `--first-a-begin IDX`
  最初の A スイープの開始インデックス
* `--a-len N`
  各 A スイープのレコード数（全イテレーションで固定）
* `--b-len N`
  各 B チャンクのレコード数（全イテレーションで固定）
* `--iterations N`
  outer iteration の総数
* `--a-pos RATIO`
  各 A スイープ内の挿入位置（0.0〜1.0）
* `--b-ratio RATIO`
  各 B チャンクの挿入割合（0.0〜1.0）
* オプション
  * `--every N`
    N イテレーションに1回だけ挿入（デフォルト: 1 = 毎回挿入）
    - `--every 1`: 全イテレーションに挿入
    - `--every 8`: 8回に1回（iter 0, 8, 16, ... に挿入）
    - `--every 0`: 挿入しない（検証用）
  * `--dry-run`
    出力せず計算結果のみ表示

### 内部計算

各イテレーション i (0 <= i < iterations) について:

```
iter_len = a_len + b_len
a_begin[i] = first_a_begin + i * iter_len
a_end[i] = a_begin[i] + a_len
b_begin[i] = a_end[i]
b_end[i] = b_begin[i] + b_len

# 挿入するかどうかの判定（--every オプション）
if (every == 0 || i % every != 0):
    skip this iteration

insert_at[i] = a_begin[i] + (int)(a_len * a_pos)
b_insert_len = (int)(b_len * b_ratio)
src_begin[i] = b_begin[i]
src_end[i] = b_begin[i] + b_insert_len
```

### 実装方針

**1パス + メモリバッファ方式:**

1. 全挿入位置を事前計算し、`insert_at[]` 配列を作成
2. 元トレースを先頭から順に読みながら:
   - 次の挿入位置に達したら:
     - 対応する B チャンクを seek して読み込み
     - 挿入レコードを出力
     - 元の位置に戻って続行
   - それ以外は元レコードをそのまま出力

**計算量**: O(入力レコード数 + 挿入総レコード数) ≈ O(N)

**メモリ使用量**: 1イテレーション分の B 挿入レコード (b_len × b_ratio × 64 bytes)

### 典型的な使用例

```bash
# 構造情報（事前に特定済み）
# - 最初のA開始: idx=322141
# - A長さ: 28679 records
# - B長さ: 20476 records
# - イテレーション数: 4096 (推定)

# ケース1: 8回に1回だけ挿入（間引きモード、推奨の開始点）
./trace_insert_all_iters \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out ../results/wp_every8_a0.5_b1.0.trace \
    --first-a-begin 322141 \
    --a-len 28679 --b-len 20476 \
    --iterations 4096 \
    --a-pos 0.5 --b-ratio 1.0 \
    --every 8

# ケース2: 全イテレーションで挿入（--every 1 または省略）
./trace_insert_all_iters \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out ../results/wp_all_iters_a0.5_b1.0.trace \
    --first-a-begin 322141 \
    --a-len 28679 --b-len 20476 \
    --iterations 4096 \
    --a-pos 0.5 --b-ratio 1.0

# ケース3: 間引き率を変えたパラメータスイープ
for every in 1 2 4 8 16; do
    ./trace_insert_all_iters \
        --in ../wp_trace \
        --out ../results/sweep_every${every}.trace \
        --first-a-begin 322141 \
        --a-len 28679 --b-len 20476 \
        --iterations 4096 \
        --a-pos 0.5 --b-ratio 1.0 \
        --every $every
done

# ケース4: a_pos, b_ratio, every の3次元スイープ
for a_pos in 0.0 0.5 1.0; do
    for b_ratio in 0.5 1.0; do
        for every in 1 8; do
            ./trace_insert_all_iters \
                --in ../wp_trace \
                --out ../results/sweep_a${a_pos}_b${b_ratio}_e${every}.trace \
                --first-a-begin 322141 \
                --a-len 28679 --b-len 20476 \
                --iterations 4096 \
                --a-pos $a_pos --b-ratio $b_ratio \
                --every $every
        done
    done
done
```

### 出力情報

```
# Input: ../wp_A64KB_B64MB_chunk32KB_stride16_os2
# Total input records: 201707970
# Structure:
#   first_a_begin = 322141
#   a_len = 28679, b_len = 20476
#   iter_len = 49155
#   iterations = 4096
# Parameters: a_pos=0.5, b_ratio=1.0, every=8
# Per-iteration insert: 20476 records at A+14339
# Active iterations: 512 (every 8th of 4096)
# Total insertions: 512 × 20476 = 10,483,712 records
# Output records: 201707970 + 10483712 = 212,191,682
```

### 出力トレースの構造

```
元:
[A0] → [B0] → [A1] → [B1] → ... → [A_N-1] → [B_N-1]

挿入後 (a_pos=0.5, b_ratio=1.0):
[A0前半] → [B0コピー] → [A0後半] → [B0] →
[A1前半] → [B1コピー] → [A1後半] → [B1] →
... →
[A_N-1前半] → [B_N-1コピー] → [A_N-1後半] → [B_N-1]
```

### 検証方法

```bash
# 1. ファイルサイズ確認
expected_size=$((201707970 + 4096 * 20476))
actual_size=$(($(stat -c%s output.trace) / 64))
echo "Expected: $expected_size, Actual: $actual_size"

# 2. 最初のイテレーションの挿入を確認
./trace_inspect --trace output.trace --start 336478 --max 5
# idx=336480 から B アクセス (IP=0x4008b3) が見えるはず

# 3. 2番目のイテレーションの挿入を確認
# 元の2番目のA開始: 322141 + 49155 = 371296
# 挿入によるシフト: +20476
# 新しい2番目のA開始: 371296 + 20476 = 391772
# 2番目の挿入位置: 391772 + 14339 = 406111
./trace_inspect --trace output.trace --start 406109 --max 5
```

### 制約事項

* 全イテレーションで A/B の長さが固定であることを前提
* イテレーション数は呼び出し側で指定（自動検出は将来課題）
* 大規模トレースでは出力ファイルサイズに注意（元の1.4倍程度になりうる）

### 段階的な検証（推奨順序）

いきなり全イテレーション (4096回) をやる前に、以下の順序で検証することを推奨:

1. **Phase 3.6 で単一挿入**: 1イテレーションだけで動作確認
2. **間引きモード**: `--every 8` や `--every 16` で間引いて効果の方向性を確認
3. **全イテレーション**: `--every 1` で本番実行

間引きモードで「効く方向かどうか」の感触が出れば、全イテレーションは確認用で十分な場合が多い。

---

## 5. 将来的な方向性メモ（まだやらなくて良いが、意識だけしておく）

この SPEC は「ステップ1〜4」をやるための最小バージョン。
余裕が出てきたら次を検討する。

1. **小さい切り出しトレースの生成**

   * `trace_slice --begin --end` のようなツールで対象範囲だけ抜き出し
   * フェーズ1〜3を軽く回す（デバッグ速度を上げる）

2. **ループ検出の自動化**

   * 今は人間が B チャンク範囲を指定する前提
   * 将来は IP の繰り返しやアドレスストライドで自動同定

3. **ChampSim ランとの統合**

   * 改造前後トレースを自動で

     * 解凍
     * ChampSim 実行
     * IPC/MPKI 集計
   * を `scripts/` にまとめる

4. **マイクロアーキとの橋渡し**

   * trace surgery の結果から

     * 「どれだけ前倒しできればどれだけ効くか」
   * の目安を作り、wrong-path 機構の設計に繋げる

当面は、フェーズ1〜4を **Cで自力で触れるレベルに落とすこと**を最優先とする。

