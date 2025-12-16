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
./trace_inspect --trace <PATH> [--max N]

# 例: 先頭100レコードを表示（デフォルト）
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride1_os2

# 例: 先頭1000レコードを表示
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride1_os2 --max 1000
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
idx,kind,ip,addr
257902,store,0x40076e,0x147a2a0
257909,store,0x40076e,0x147a2a8
315267,load,0x4007e0,0x147a2a0
...
```

- `idx`: レコード番号 (0始まり)
- `kind`: `load` (読み込み) または `store` (書き込み)
- `ip`: 命令アドレス
- `addr`: アクセス先アドレス

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

## 今後の予定

- **Phase 3**: `trace_overwrite_range` - トレースの範囲コピー/上書き
