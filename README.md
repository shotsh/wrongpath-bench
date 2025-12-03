# wrongpath-bench

ループ本体と「でかい配列アクセス」を使って、キャッシュ階層と DRAM へのデマンドアクセスを制御しやすい形で観測するための小さなマイクロベンチマークです。

目的は主に:

* **L1/L2/DRAM のミス頻度 (MPKI/PKI)** をざっくり把握する
* **アクセスパターン (dense / strided)** を切り替えて、

  * DRAM まで到達するデマンドアクセス
  * L1/L2 ヒット率
    がどう変わるかを見る
* 将来的に **wrong-path prefetch / loop-tail 実験用のトレース生成** の元ネタにする

`perf stat` をラップする Python スクリプトで、MPKI や DRAM fill PKI を自動計算できます。

---

## ファイル構成

* `benchmark.c`
  L1 を吹き飛ばす小さい配列 `A` と、大きな配列 `B` に対するアクセスループ本体。
* `scripts/run_perf_mpki.py`
  `perf stat` を呼び出し、L1/L2 miss rate, MPKI, DRAM fill PKI, IPC を計算して表示するラッパ。

---

## ビルド方法

普通に `gcc` で OK です。

```bash
gcc -O3 -march=native -Wall -o benchmark benchmark.c
```

---

## ベンチマークの挙動

### コマンドライン引数

```bash
./benchmark A_bytes B_bytes chunk_bytes [access_mode] [stride_elems] [outer_scale]
```

* `A_bytes`

  * 小さい配列 `A` のバイト数
  * 毎イテレーションで **全要素を走査**して L1 を吹き飛ばす役
* `B_bytes`

  * 論理的な `B` のサイズ (バイト)
  * これを `chunk_bytes` で割って **外側ループ回数**を決める
* `chunk_bytes`

  * B を「チャンク」に刻むサイズ (バイト)
  * `B_bytes` は `chunk_bytes` の倍数である必要がある
* `access_mode` (オプション, デフォルト 0)

  * `0` = dense アクセス (連続アクセス)
  * `1` = strided アクセス
* `stride_elems` (オプション, デフォルト 8)

  * `access_mode=1` のときのストライド (double 要素単位)
  * `access_mode=0` のときも **配列 B の確保サイズ**には効く (後述)
* `outer_scale` (オプション, デフォルト 1)

  * ループ全体を何周分まわすか
  * **実効 outer ループ回数をスケールするノブ**

### 配列サイズとループ回数

コード上の主なパラメータは:

* `A_elems = A_bytes / sizeof(double)`
* `B_elems = B_bytes / sizeof(double)`  …論理的な B の要素数
* `chunk_elems = chunk_bytes / sizeof(double)`

制約:

* `A_elems > 0`
* `B_elems > 0`
* `chunk_elems > 0`
* `B_elems % chunk_elems == 0`

論理的な B をチャンクに分割したとき:

```text
base_outer_iters = B_elems / chunk_elems
```

* `outer_scale` で全体を増やして

```text
outer_iters = base_outer_iters * outer_scale
```

* 実際のカーネル内ループは:

```c
for (outer = 0; outer < outer_iters; outer++) {
    // A 全体をなめて L1 を吹き飛ばす
    for (i = 0; i < A_elems; i++) { ... }

    size_t base = outer * chunk_elems * stride_elems;

    // chunk_elems 回だけ必ずループ
    for (j = 0; j < chunk_elems; j++) {
        size_t idx = base + j * stride_elems;
        sum += B[idx];
    }
}
```

### dense / strided の切り替え

* `access_mode == 0` (dense)

  * `stride_elems` は **実効的には 1** に固定される
  * ただし `user_stride` は B の確保サイズにだけ効く
* `access_mode == 1` (strided)

  * `stride_elems = user_stride` がそのまま使われる

まとめると:

* 論理 B サイズ (`B_bytes`) と `chunk_bytes` から **outer/inner のループ回数**を決定
* `stride_elems` の値だけを変えても、

  * outer/inner のループ回数は同じ
  * **実行命令数はほぼ同じ**
  * ただしアクセスパターンとワーキングセットが変わる

### B の確保サイズ

実際に確保する B の要素数は

```c
B_elems_alloc = B_elems * user_stride * outer_scale;
```

* dense のとき (`access_mode=0`)

  * `stride_elems = 1` でアクセス
  * ただし `user_stride` と `outer_scale` を掛けたぶん **余裕を持って確保**している
* strided のとき (`access_mode=1`)

  * `stride_elems = user_stride`
  * 必要なサイズが `B_elems * user_stride * outer_scale` で、
    ちょうど `B_elems_alloc` と一致

この設計にしている理由:

* **同じループ回数・同じ関数**のまま
* `stride_elems` だけを変えて、

  * 命令数はほぼ不変
  * メモリアクセスのパターンと DRAM 到達率だけ変える
    という比較がしやすくするため

---

## 典型的なパラメータ例

* `A_bytes`

  * コアの L1D より少し大きい程度
  * 例: L1D = 32 KiB なら `A_bytes = 32*1024`
* `B_bytes`

  * LLC よりずっと大きくして、DRAM に十分届くようにする
  * 例: `B_bytes = 512*1024*1024` (512 MiB)
* `chunk_bytes`

  * L2 くらいのオーダー、あるいはそのサブセット
  * 例: `chunk_bytes = 512*1024` (512 KiB)
* `access_mode` / `stride_elems`

  * dense: `access_mode = 0`, `stride_elems` は何でも良い (確保サイズだけに効く)
  * strided: `access_mode = 1`, 例えば

    * `stride_elems = 8` (1 ラインに 1 要素程度)
    * `stride_elems = 16` など
* `outer_scale`

  * ループ本体だけの IPC / MPKI を強調したいときに大きめにする
  * 例: `outer_scale = 100`

---

## perf ラッパースクリプト (`run_perf_mpki.py`)

### 概要

このスクリプトは

* `perf stat -x,` を呼び出して CSV を取る
* 以下のカウンタをパース

  * `cycles`
  * `instructions`
  * `L1-dcache-loads`
  * `L1-dcache-load-misses`
  * `l2_cache_accesses_from_dc_misses`
  * `l2_cache_misses_from_dc_misses`
  * DRAM 関連イベント（ノードによって違う）
* そこから

  * L1 miss rate
  * L2 miss rate (L1D miss を母数)
  * L1 / L2 MPKI
  * DRAM fill PKI (demand fill per 1K instructions)
  * IPC

を計算して表示します。

### 使い方

形式:

```bash
./scripts/run_perf_mpki.py [--node demeter|artemis] <binary> [binary-args...]
```

#### 例 1: n05-demeter (EPYC, `ls_refills_from_sys.*` があるノード)

`--node` を省略すると `demeter` モードになります。

```bash
./scripts/run_perf_mpki.py ./benchmark 32768 536870912 524288 1 16 100
```

このモードでは DRAM fill を

* `ls_refills_from_sys.ls_mabresp_lcl_dram`
* `ls_refills_from_sys.ls_mabresp_rmt_dram`

の合計としてカウントします。

#### 例 2: n07-artemis (`ls_dmnd_fills_from_sys.mem_io_local` のみ)

```bash
./scripts/run_perf_mpki.py --node artemis ./benchmark 32768 536870912 524288 1 16 100
```

このモードでは DRAM fill を

* `ls_dmnd_fills_from_sys.mem_io_local` の値を **local 分として使用**
* remote は 0 とみなす

として扱います。

### 出力フォーマット

1. `perf` の生の stderr (CSV)
2. パースしたカウンタの一覧
3. miss rate と IPC
4. MPKI / PKI

例:

```text
=== perf raw output (stderr) ===
... (perf の出力そのまま) ...

=== Parsed counters ===
node                    : demeter
cycles                 : 1921158234
instructions           : 4649560413
L1-dcache-loads        : 78971700
L1-dcache-load-misses  : 134840330
l2_cache_accesses_from_dc_misses : 134891535
l2_cache_misses_from_dc_misses   : 13975630
Demand DRAM fills (L1D): 57480933 (local=57480933, remote=0)

=== Rates / IPC ===
L1 miss rate           : 170.75 %
L2 miss rate (on L1D misses): 10.36 %
IPC                    : 2.420

=== Per-1K-instruction metrics (MPKI/PKI) ===
L1 MPKI                : 28.954
L2 MPKI                : 3.029
Demand DRAM fills (L1D) PKI : 12.464
```

ここで

* **L1 MPKI / L2 MPKI**

  * それぞれ「L1/L2 ミス数 / 1000 命令」
* **DRAM fill PKI**

  * `ls_refills_from_sys...` / `ls_dmnd_fills_from_sys...` の合計を
    「デマンドの DRAM フィル回数 / 1000 命令」として解釈

---

## 何を見ればいいか

* **L1 MPKI / L2 MPKI**

  * stride を大きくすると通常は増加
  * outer_scale を大きくすると、初期化の影響が薄まり **ループ本体のミス特性**が素直に出る
* **DRAM fill PKI**

  * 「このループがどれくらい DRAM を叩いているか」のざっくり指標
  * wrong-path prefetch の評価では

    * DRAM fill の絶対数は大きく変わらなくても
    * **デマンド vs プリフェッチの比率**が変わることを見たい
      → そのときのベースラインとして使う

---

## 注意・限界

* これは **かなりシンプルなストリーム系マイクロベンチ**です。

  * TLB, スnoop, コヒーレンシ, 仮想化など、現実の OS/アプリでは効いてくる要素はほぼ入っていません。
* `perf` イベント名は CPU 世代やカーネルによって違うので、

  * 新しいノードで使うときは一度 `perf list` を確認して、
  * 必要なら `run_perf_mpki.py` のイベント定義を増やす前提です。
* `perf_event_paranoid` の値によっては、そもそもカーネルで perf が制限されていて使えない場合もあります。

---

この README をベースにしておけば、後で

* 「この数字おかしくない？」となったときの前提確認
* 新しいノードに持っていったときのポーティング作業

がだいぶ楽になるはずです。必要なら「推奨パラメータセット (dense/stride × outer_scale)」を表にして追記するのもアリです。
