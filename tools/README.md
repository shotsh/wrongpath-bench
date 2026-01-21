# Trace Surgery Tools

ChampSim バイナリトレースを操作するためのツール群。

## ビルド

```bash
cd tools
make
```

## ツール一覧

### trace_inspect (Phase 1)

トレースファイルを読み込み、レコードの内容を人間が読める形式で表示する。

```bash
# 使い方
./trace_inspect --trace <PATH> [--max N] [--start IDX]

# 例: 先頭100レコードを表示（デフォルト）
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride1_os2

# 例: 先頭1000レコードを表示
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride1_os2 --max 1000

# 例: idx=350820 から10レコードを表示
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 --start 350820 --max 10
```

#### 出力フォーマット

```
idx=<レコード番号> ip=<命令アドレス> src_mem=[<読込アドレス,...>] dst_mem=[<書込アドレス,...>]
```

- `src_mem` / `dst_mem` はゼロでないアドレスのみ表示
- `sizeof(input_instr) = 64 bytes`

#### 出力例

```
# Trace file: ../wp_A64KB_B64MB_chunk32KB_stride1_os2
# sizeof(input_instr) = 64 bytes
# Displaying up to 20 records
#
idx=0 ip=0x7f541cbe0f10 src_mem=[] dst_mem=[]
idx=1 ip=0x7f541cbe0f13 src_mem=[] dst_mem=[0x87778b78]
idx=2 ip=0x7f541cbe1bf0 src_mem=[] dst_mem=[]
...
```

## トレースフォーマット

ChampSim の `inc/trace_instruction.h` にある `struct input_instr` と同じバイナリレイアウト:

```c
#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES 4

struct input_instr {
    uint64_t ip;
    uint8_t  is_branch;
    uint8_t  branch_taken;
    uint8_t  destination_registers[NUM_INSTR_DESTINATIONS];
    uint8_t  source_registers[NUM_INSTR_SOURCES];
    uint64_t destination_memory[NUM_INSTR_DESTINATIONS];
    uint64_t source_memory[NUM_INSTR_SOURCES];
};
```

### find_b_accesses (Phase 2)

トレースをスキャンし、配列Bのアドレス範囲内へのメモリアクセスをCSV形式で出力する。

```bash
# 使い方
./find_b_accesses --trace <PATH> --b-base <ADDR> --b-size <BYTES> [--max-hits N]

# 例: Bのベースアドレスとサイズを指定
./find_b_accesses --trace ../backup/wp_A64KB_B64MB_chunk32KB_stride1_os2 \
    --b-base 0x147a000 --b-size 67108864 --max-hits 100
```

#### オプション

| オプション | 説明 |
|-----------|------|
| `--trace PATH` | トレースファイルのパス (必須) |
| `--b-base ADDR` | 配列Bのベースアドレス (16進数, 必須) |
| `--b-size BYTES` | 配列Bのサイズ (バイト, 必須) |
| `--max-hits N` | 報告するアクセス数の上限 (デフォルト: 無制限) |

#### 出力フォーマット (CSV)

```csv
idx,kind,ip,addr,offset
350820,load,0x4008b3,0xc33fd010,0x0
350825,load,0x4008b3,0xc33fd090,0x80
350830,load,0x4008b3,0xc33fd110,0x100
...
```

- `idx`: レコード番号 (0始まり)
- `kind`: `load` (読み込み) または `store` (書き込み)
- `ip`: 命令アドレス
- `addr`: アクセス先アドレス
- `offset`: `addr - b_base` (配列内オフセット、目視検証用)

## カーネル区間の特定手順

ベンチマークの printf 出力からトレース内の配列 B アクセスを特定するまでの具体的な手順。

### Step 1: トレース生成時に B のアドレスを記録

```bash
# Pin tracer でトレース生成
$PIN_ROOT/pin -t path/to/champsim_tracer.so \
    -o >(xz -T4 > wp_A64KB_B64MB_chunk32KB_stride16_os2.xz) \
    -- ./benchmark_trace 65536 67108864 32768 1 16 2

# 出力例:
#   A=0xfc62a0
#   B=0x7f9cc33fd010
#   B_alloc_bytes = 1073741824
```

**記録すべき値:**
- `B=0x7f9cc33fd010` (Bのベースアドレス)
- `B_alloc_bytes = 1073741824` (Bの確保サイズ)

### Step 2: アドレスの下位32ビットを取得

ChampSim の Pin tracer はメモリアドレスを**下位32ビット**で記録する。
そのため、64ビットアドレスから下位32ビットを抽出する。

```
B = 0x7f9cc33fd010
         ^^^^^^^^
下位32ビット = 0xc33fd010
```

**計算方法:**
```bash
# Python で計算
python3 -c "print(hex(0x7f9cc33fd010 & 0xFFFFFFFF))"
# 出力: 0xc33fd010
```

### Step 3: find_b_accesses で B アクセスを検索

```bash
# トレースを解凍
xz -dk wp_A64KB_B64MB_chunk32KB_stride16_os2.xz

# B アクセスを検索（下位32ビットのアドレスを使用）
./find_b_accesses \
    --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --b-base 0xc33fd010 \
    --b-size 1073741824 \
    --max-hits 100
```

### Step 4: カーネル区間の特定

出力例:
```csv
idx,kind,ip,addr
350820,load,0x4008b3,0xc33fd010
350825,load,0x4008b3,0xc33fd090
350830,load,0x4008b3,0xc33fd110
...
```

**確認ポイント:**

1. **IP アドレス**: `0x4008b3` のような低いアドレス → ベンチマーク本体の命令
   - `0x7f...` のような高いアドレス → libc 等の初期化コード

2. **アクセス種別**:
   - `TRACE_MODE` では B の初期化がスキップされるため、`load` のみ
   - 通常ビルドでは初期化フェーズに `store` が含まれる

3. **アドレスのストライド**:
   - stride=16 の場合: 0x80 (128バイト = 16 × 8バイト) 刻み

### Step 5: 全体統計の取得

```bash
# 全 B アクセスを CSV に出力
./find_b_accesses \
    --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --b-base 0xc33fd010 \
    --b-size 1073741824 > b_accesses.csv 2>&1

# 統計情報
wc -l b_accesses.csv              # 総アクセス数
grep -c ',load,' b_accesses.csv   # load 数
grep -c ',store,' b_accesses.csv  # store 数

# IP 別の集計
cut -d',' -f3 b_accesses.csv | sort | uniq -c | sort -rn | head -5
```

### 実例: wp_A64KB_B64MB_chunk32KB_stride16_os2 の解析結果

| 項目 | 値 |
|------|-----|
| ベンチマークパラメータ | `65536 67108864 32768 1 16 2` |
| B 出力アドレス | `0x7f9cc33fd010` |
| 下位32ビット | `0xc33fd010` |
| B_alloc_bytes | 1,073,741,824 (1GB) |
| 最初の B アクセス | idx=350,820 |
| 最後の B アクセス | idx=196,621,978 |
| カーネル IP | `0x4008b3` |
| 総 B アクセス数 | 15,925,440 |
| load / store | 15,925,440 / 0 |

**B アクセスが load のみである理由:**

1. **TRACE_MODE**: B の初期化 (`init_array(B, ...)`) がスキップされるため、初期化時の store がない
2. **コンパイラ最適化**: カーネル内の `sum += B[idx]` において、`sum` はレジスタ (xmm0) に保持される
   - B からの load は発生する
   - `sum` への store はメモリに書き出されない（レジスタ内で完結）
   - ループ終了後に `sink` へ 1 回だけ store される

```asm
# 実際のアセンブリ (objdump -d benchmark_trace)
4008b3: vaddsd (%rcx),%xmm0,%xmm0   # B[idx]をload、xmm0に加算
4008b7: add    %rsi,%rcx             # ポインタ更新
...
4008e1: vmovsd %xmm2,0x201787(%rip)  # ループ終了後、sinkにstore (1回のみ)
```

## 配列Aアクセスの特定

配列Bだけでなく、配列Aのアクセスも特定する必要がある場合の手順。

### カーネル構造

`benchmark.c` の `run_kernel` 関数は以下の構造を持つ:

```c
for (size_t outer = 0; outer < outer_iters; outer++) {
    // 1) 配列A sweep (L1キャッシュを乱す)
    for (size_t i = 0; i < A_elems; i++) {
        sum += A[i];  // ← IP: 0x400880, 0x40088c
    }

    // 2) 配列B chunk アクセス
    for (size_t j = 0; j < elems_per_iter; j++) {
        sum += B[base + j * stride_elems];  // ← IP: 0x4008b3
    }
}
```

### Step 1: ディスアセンブリでIPアドレスを確認

```bash
# カーネル部分のディスアセンブリを確認
objdump -d benchmark_trace | grep -A100 '400880'
```

出力例:
```asm
400880: vmovsd (%rdx),%xmm1          # A[i] の load (偶数インデックス)
400884: add    $0x10,%rdx
400888: vaddsd %xmm0,%xmm1,%xmm0
40088c: vmovsd -0x8(%rdx),%xmm1      # A[i+1] の load (ループアンロール)
...
4008b3: vaddsd (%rcx),%xmm0,%xmm0    # B[idx] の load
```

**重要: コンパイラ最適化によりループがアンロールされ、Aアクセスは2つのIPで行われる**

### Step 2: trace_inspect でAアクセスを確認

```bash
# Aアクセス (IP: 0x400880, 0x40088c) をフィルタ
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 --max 500000 \
    | grep -E 'ip=0x400880|ip=0x40088c' | head -20
```

出力例:
```
idx=322141 ip=0x400880 src_mem=[0xfc62a0] dst_mem=[]
idx=322144 ip=0x40088c src_mem=[0xfc62a8] dst_mem=[]
idx=322148 ip=0x400880 src_mem=[0xfc62b0] dst_mem=[]
idx=322151 ip=0x40088c src_mem=[0xfc62b8] dst_mem=[]
...
```

### Step 3: Aのアドレス範囲を特定

```bash
# ユニークなアドレスを抽出
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 --max 10000000 \
    | grep -E 'ip=0x400880|ip=0x40088c' \
    | awk -F'src_mem=\\[' '{print $2}' | cut -d']' -f1 \
    | sort -u > /tmp/a_addrs.txt

# 範囲を確認
echo "最小アドレス: $(head -1 /tmp/a_addrs.txt)"
echo "最大アドレス: $(tail -1 /tmp/a_addrs.txt)"
echo "ユニークアドレス数: $(wc -l < /tmp/a_addrs.txt)"
```

### Step 4: アクセス統計の取得

```bash
# Aアクセス総数
A_COUNT=$(./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 --max 100000000 \
    | grep -cE 'ip=0x400880|ip=0x40088c')
echo "Aアクセス総数: $A_COUNT"

# Bアクセス総数（比較用）
B_COUNT=$(./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 --max 100000000 \
    | grep -c 'ip=0x4008b3')
echo "Bアクセス総数: $B_COUNT"

# outer iterations の計算
echo "推定 outer iterations: $((A_COUNT / 8192))"
```

### 配列AとBのアクセス比較表

| 項目 | 配列A | 配列B |
|------|-------|-------|
| **IP** | `0x400880`, `0x40088c` | `0x4008b3` |
| **アドレス範囲** | `0xfc62a0` ~ `0xfd6298` | `0xc33fd010` ~ ... |
| **サイズ** | 65,536 bytes (64KB) | 67,108,864 bytes (64MB) × stride |
| **ストライド** | 8 bytes (連続) | 128 bytes (stride=16 × 8) |
| **1 outer iter** | 8,192 アクセス | 4,096 アクセス |

### 実例: wp_A64KB_B64MB_chunk32KB_stride16_os2

| 項目 | 配列A | 配列B |
|------|-------|-------|
| ベースアドレス (printf出力) | `0xfc62a0` | `0x7f9cc33fd010` |
| トレース内アドレス | `0xfc62a0` | `0xc33fd010` (下位32bit) |
| サイズ | 65,536 bytes | 1,073,741,824 bytes |
| IP | `0x400880`, `0x40088c` | `0x4008b3` |
| 最初のアクセス idx | 322,141 | 350,820 |
| アクセス総数 | 1,613,824 | 15,925,440 |

**注意:** 配列Aのアドレスは下位32ビットでも元のアドレスと同じ（スタック/ヒープの低いアドレスに配置されるため）。

### カーネルのアクセスパターン

```
[A sweep (8192 loads)] → [B chunk (4096 loads)] → [A sweep] → [B chunk] → ...
     idx=322141~          idx=350820~              次のiter
```

各 outer iteration で:
1. 配列A全体を連続アクセス (8,192要素)
2. 配列Bの1チャンクをストライドアクセス (4,096要素)

### trace_overwrite_range (Phase 3)

トレースの指定範囲を別の位置にコピー（上書き）する。トレース全体の長さは変わらない。

```bash
# 使い方
./trace_overwrite_range --in <INPUT> --out <OUTPUT> \
    --src-begin I --src-end J --dst-begin K [--dry-run]

# 例: idx=350820-350920 の100レコードを idx=322141 に上書きコピー
./trace_overwrite_range \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out /tmp/modified.trace \
    --src-begin 350820 --src-end 350920 --dst-begin 322141

# 例: dry-run で範囲検証のみ（出力ファイルを作成しない）
./trace_overwrite_range \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --src-begin 350820 --src-end 350920 --dst-begin 322141 \
    --dry-run
```

#### オプション

| オプション | 説明 |
|-----------|------|
| `--in PATH` | 入力トレースファイル (必須) |
| `--out PATH` | 出力トレースファイル (必須、`--dry-run` 時は不要) |
| `--src-begin I` | コピー元の開始インデックス (含む、必須) |
| `--src-end J` | コピー元の終了インデックス (含まない、必須) |
| `--dst-begin K` | コピー先の開始インデックス (必須) |
| `--dry-run` | 範囲検証のみ、出力ファイルを作成しない |

#### 動作

1. トレースを先頭から順に読み込む
2. `idx` が `[dst_begin, dst_begin + (src_end - src_begin))` 範囲内の場合:
   - 元のレコードを破棄し、`[src_begin, src_end)` のレコードを順に出力
3. それ以外の場合:
   - 元のレコードをそのまま出力
4. 出力トレースの総レコード数は入力と同じ（上書きモード）

#### 検証方法

```bash
# 元トレースの src 範囲を確認
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --start 350820 --max 5

# overwrite 後の dst 範囲を確認（src と同じ内容になるはず）
./trace_inspect --trace /tmp/modified.trace \
    --start 322141 --max 5
```

#### 典型的なユースケース

**Bチャンクの先読み効果を検証:**

1. `find_b_accesses` で最初のBチャンクの範囲を特定
2. 配列Aアクセスの途中（または直前）を dst として指定
3. Bチャンクを dst に上書きコピー
4. ChampSim で元トレースと改造トレースの IPC を比較

```bash
# 例: Bの最初のチャンク (idx=350820~354916) を A sweep の途中 (idx=330000) に上書き
./trace_overwrite_range \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out ../wp_modified.trace \
    --src-begin 350820 --src-end 354916 --dst-begin 330000
```

**注意:** この上書きは一次近似であり、本来の wrong-path 動作とは異なる。
キャッシュウォーミング効果の簡易検証が目的。

### trace_insert_range (Phase 3.5)

トレースの指定範囲を別の位置に「挿入」する。上書きモードと異なり、元のレコードはすべて保持され、トレース長が増加する。

```bash
# 使い方
./trace_insert_range --in <INPUT> --out <OUTPUT> \
    --src-begin I --src-end J --insert-at K [--dry-run]

# 例: Aの真ん中 (idx=336480) にBの先頭14340レコードを挿入
./trace_insert_range \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out ../results/wp_inserted.trace \
    --src-begin 350820 --src-end 365160 --insert-at 336480

# 例: dry-run で範囲検証とインデックスマッピング確認
./trace_insert_range \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --src-begin 350820 --src-end 365160 --insert-at 336480 \
    --dry-run
```

#### オプション

| オプション | 説明 |
|-----------|------|
| `--in PATH` | 入力トレースファイル (必須) |
| `--out PATH` | 出力トレースファイル (必須、`--dry-run` 時は不要) |
| `--src-begin I` | コピー元の開始インデックス (含む、必須) |
| `--src-end J` | コピー元の終了インデックス (含まない、必須) |
| `--insert-at K` | 挿入位置 (このインデックスの直前に挿入、必須) |
| `--dry-run` | 範囲検証のみ、出力ファイルを作成しない |

#### 動作

1. src範囲 `[src_begin, src_end)` をメモリに読み込む
2. トレースを先頭から順に読み込む
3. `idx == insert_at` に達したら:
   - src範囲のレコードを出力（挿入）
   - 元のレコードも出力
4. 出力トレースの総レコード数 = 入力 + (src_end - src_begin)

#### 出力インデックスのマッピング

```
入力: [0, ..., insert_at-1, insert_at, ..., total-1]
出力: [0, ..., insert_at-1, INSERTED..., insert_at+len, ..., total+len-1]
                            ^^^^^^^^^^
                            挿入された部分
```

#### 検証方法

```bash
# ファイルサイズで挿入を確認
ls -l original.trace inserted.trace
# 差分 = 挿入レコード数 × 64 bytes

# 挿入位置の内容を確認
./trace_inspect --trace inserted.trace --start 336478 --max 5
# idx=336478, 336479: 元のAアクセス
# idx=336480~: 挿入されたBアクセス

# 元のレコードがシフトしていることを確認
./trace_inspect --trace inserted.trace --start $((336480 + 14340)) --max 3
# 元の336480のレコードが表示される
```

#### 典型的なユースケース

**Bチャンクの先読み効果を検証（挿入モード）:**

```bash
# Aの真ん中にBの先頭部分を挿入（Aの後半と同じ長さ）
# A sweep: idx=322141 ~ 350819 (28679 records)
# A真ん中: idx=336480
# A後半長さ: 14340 records

./trace_insert_range \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out ../results/wp_inserted_A_mid_B.trace \
    --src-begin 350820 --src-end 365160 \
    --insert-at 336480
```

**挿入後のトレース構造:**
```
元:   [A sweep (全体)] → [B chunk]
挿入: [A前半] → [B挿入] → [A後半] → [B chunk]
```

**上書きモードとの違い:**
- 上書き: 元のレコードが消える、トレース長は変わらない
- 挿入: 元のレコードはすべて保持、トレース長が増加

## trace_insert_all_iters (Phase 4)

全 outer iteration に対して、各イテレーションのBチャンクをAの指定位置に一括挿入する。

```bash
# 使い方
./trace_insert_all_iters --in <INPUT> --out <OUTPUT> \
    --first-a-begin IDX --a-len N --b-len N --iterations N \
    --a-pos RATIO --b-ratio RATIO [--every N] [--dry-run]
```

#### オプション

| オプション | 説明 |
|-----------|------|
| `--in PATH` | 入力トレースファイル (必須) |
| `--out PATH` | 出力トレースファイル (必須、`--dry-run` 時は不要) |
| `--first-a-begin IDX` | 最初のAスイープの開始インデックス (必須) |
| `--a-len N` | 各Aスイープのレコード数 (必須) |
| `--b-len N` | 各Bチャンクのレコード数 (必須、ループオーバーヘッド含む) |
| `--iterations N` | outer iteration の総数 (必須) |
| `--a-pos RATIO` | Aスイープ内の挿入位置 (0.0〜1.0、必須) |
| `--b-ratio RATIO` | Bチャンクの挿入割合 (0.0〜1.0、必須) |
| `--every N` | N イテレーションに1回だけ挿入 (デフォルト: 1 = 毎回) |
| `--dry-run` | 範囲検証のみ、出力ファイルを作成しない |

#### 重要: パラメータ値について

**トレース構造にはループオーバーヘッドが存在する:**
- B の最後のロードアクセス後、次の A の最初のロードアクセスまでに **11命令** のオーバーヘッドがある
- `find_b_accesses` で検出される実Bアクセス数と `b_len` は異なる

**wp_A64KB_B64MB_chunk32KB_stride16_os2 の正しいパラメータ:**

| パラメータ | 値 | 説明 |
|-----------|-----|------|
| first-a-begin | 322141 | 最初のAスイープ開始位置 |
| a-len | 28679 | Aスイープのレコード数 |
| b-len | **20487** | Bチャンク + ループオーバーヘッド (20476 + 11) |
| iterations | 4096 | outer iteration 数 |

#### 挿入後のトレース構造

```
元 (iter i):
[A sweep] → [B chunk (iter i)]

挿入後 (iter i):
[A前半] → [B chunk (iter i) コピー] → [A後半] → [B chunk (iter i)]
```

各イテレーションで、そのイテレーション自身のBチャンクがAの指定位置に挿入される。
同じBアクセスが2回出現する形になる（先読み効果のシミュレーション）。

---

## 実例: 全イテレーションへのB挿入トレース生成

`wp_A64KB_B64MB_chunk32KB_stride16_os2` を元に、全4096イテレーションでAの真ん中にBチャンクを挿入するトレースを生成する手順。

### Step 1: イテレーション数の計算

```bash
# ファイルサイズからレコード数を計算
filesize=$(stat -c%s wp_A64KB_B64MB_chunk32KB_stride16_os2)
total_records=$((filesize / 64))
echo "総レコード数: $total_records"  # 201707970

# イテレーション数を計算
first_a_begin=322141
iter_len=49166  # a_len(28679) + b_len(20487)
kernel_records=$((total_records - first_a_begin))
iterations=$((kernel_records / iter_len))
echo "イテレーション数: $iterations"  # 4096
```

### Step 2: トレース生成

```bash
# results ディレクトリを作成
mkdir -p results

# 全イテレーションでAの真ん中 (a_pos=0.5) にBチャンク全体 (b_ratio=1.0) を挿入
./tools/trace_insert_all_iters \
    --in ./wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out ./results/wp_A64KB_B64MB_chunk32KB_stride16_os2_A05_BChunk10.trace \
    --first-a-begin 322141 \
    --a-len 28679 --b-len 20487 \
    --iterations 4096 \
    --a-pos 0.5 --b-ratio 1.0 \
    --every 1
```

### Step 3: 出力確認

```bash
# ファイルサイズ確認
# 期待値: 201707970 + 4096 * 20487 = 285622722 レコード = 18,279,854,208 bytes
ls -lh results/wp_A64KB_B64MB_chunk32KB_stride16_os2_A05_BChunk10.trace
# 17G (≈ 17.02 GB)

# レコード数確認
actual_records=$(($(stat -c%s results/wp_A64KB_B64MB_chunk32KB_stride16_os2_A05_BChunk10.trace) / 64))
echo "実際のレコード数: $actual_records"  # 285622722
```

### Step 4: 挿入位置の検証

```bash
# iter 0 の挿入確認 (挿入位置: 322141 + 14339 = 336480)
./tools/trace_inspect \
    --trace results/wp_A64KB_B64MB_chunk32KB_stride16_os2_A05_BChunk10.trace \
    --start 336478 --max 5

# 期待される出力:
# idx=336478 ip=0x400884 ... ← A アクセス (挿入前)
# idx=336479 ip=0x400888 ...
# idx=336480 ip=0x4008b3 ... ← B アクセス (挿入されたBチャンク開始)
# idx=336481 ip=0x4008b7 ...
```

### 生成されたトレースのサマリ

| 項目 | 値 |
|------|-----|
| 入力ファイル | `wp_A64KB_B64MB_chunk32KB_stride16_os2` |
| 出力ファイル | `results/wp_A64KB_B64MB_chunk32KB_stride16_os2_A05_BChunk10.trace` |
| 元サイズ | 12.02 GB (201,707,970 レコード) |
| 出力サイズ | 17.02 GB (285,622,722 レコード) |
| イテレーション数 | 4096 |
| 挿入位置 (a_pos) | 0.5 (Aの真ん中) |
| 挿入量 (b_ratio) | 1.0 (Bチャンク全体 = 20,487 レコード/iter) |
| 総挿入レコード数 | 83,914,752 (4096 × 20,487) |

### トレース構造の図解

```
元トレース (iter i):
[A sweep (28679)] → [B chunk (20487)]

挿入後 (iter i):
[A前半 (14339)] → [B chunk コピー (20487)] → [A後半 (14340)] → [B chunk (20487)]

効果:
- Bチャンクへのアクセスが A sweep の途中で先に発生
- 同じBデータが2回アクセスされる（先読み + 本来のアクセス）
- キャッシュウォーミング効果をシミュレート
```
