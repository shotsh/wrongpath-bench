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

## 今後の予定

- **Phase 2**: `find_b_accesses` - 配列Bへのアクセス位置を特定
- **Phase 3**: `trace_overwrite_range` - トレースの範囲コピー/上書き
