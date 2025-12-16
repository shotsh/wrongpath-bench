# 挿入モード (trace_insert_range) 仕様案

## 1. 概要

トレースの指定範囲を別の位置に「挿入」する。
上書きモードと異なり、元のレコードはすべて保持され、トレース長が増加する。

## 2. ユースケース

### 想定シナリオ
- Aの途中（またはA終了直後）に、Bチャンクのコピーを挿入
- これにより「Bアクセスを先に実行してキャッシュを温める」効果を評価

```
元:   [A sweep 8192] → [B chunk 4096]
挿入: [A sweep 4096] → [B chunk copy] → [A sweep 4096] → [B chunk 4096]
                       ^^^^^^^^^^^^
                       挿入された部分
```

## 3. CLI 仕様案

### 案A: 単一挿入ポイント

```bash
./trace_insert_range \
    --in PATH_IN \
    --out PATH_OUT \
    --src-begin I \
    --src-end J \
    --insert-at K \
    [--dry-run]
```

| オプション | 説明 |
|-----------|------|
| `--src-begin I` | コピー元の開始インデックス (含む) |
| `--src-end J` | コピー元の終了インデックス (含まない) |
| `--insert-at K` | 挿入位置（このインデックスの直前に挿入） |

### 動作

1. idx < K のレコードをそのまま出力
2. K に達したら、src範囲 [I, J) のレコードを出力（挿入）
3. idx >= K のレコードをそのまま出力

### 出力トレースの長さ

```
出力レコード数 = 入力レコード数 + (J - I)
```

## 4. 実装方針

### 1パス実装（メモリ効率重視）

```
[Phase 1] src範囲をメモリに読み込み
[Phase 2] 入力を先頭から読みながら:
          - idx < insert_at: そのまま出力
          - idx == insert_at: src範囲を出力、その後元レコードも出力
          - idx > insert_at: そのまま出力
```

**メモリ使用量**: src範囲のレコード数 × sizeof(input_instr)

### 2パス実装（大きなsrc範囲対応）

```
[Phase 1] 入力を先頭から読み、idx < insert_at をそのまま出力
[Phase 2] 入力をseekしてsrc範囲を出力
[Phase 3] 入力をseekしてidx >= insert_at を出力
```

**メモリ使用量**: 1レコード分のみ

### 推奨: 1パス実装

- src範囲は通常 Bチャンク (4096アクセス × 5レコード ≈ 20,480レコード)
- 20,480 × 64 bytes = 1.3 MB → 現実的なメモリ使用量
- 1パスの方が実装がシンプル

## 5. 複数挿入への拡張（将来）

### 案B: 編集計画ファイル方式

複数の挿入を1回のパスで処理するため、編集計画をファイルで渡す。

```bash
./trace_edit \
    --in PATH_IN \
    --out PATH_OUT \
    --plan edit_plan.json
```

**edit_plan.json の例:**
```json
{
  "operations": [
    {"type": "insert", "src_begin": 350820, "src_end": 371295, "insert_at": 336480},
    {"type": "insert", "src_begin": 399986, "src_end": 420461, "insert_at": 385325}
  ]
}
```

### 実装方針

1. 編集計画をinsert_at順にソート
2. 入力を1パスで読みながら:
   - 次の挿入ポイントに達したら挿入を実行
   - 挿入ポイント間は元レコードをそのまま出力

**制約:**
- 挿入ポイントは元トレースのインデックスで指定
- 挿入によるインデックスシフトは呼び出し側で考慮不要（元インデックスで指定）

## 6. 注意点・制約

### src範囲と挿入位置の関係

```
ケース1: src が insert_at より後ろ（典型的）
  元: [... A ...][... B ...]
               ^insert_at   ^src
  → OK: Bを先にコピーしてAの途中に挿入

ケース2: src が insert_at より前
  元: [... X ...][... Y ...]
      ^src      ^insert_at
  → OK: Xのコピーを後ろに挿入（あまり使わないが可能）

ケース3: src と insert_at が重なる
  → 警告を出すが、動作は保証（元のsrcを先に読み込むため）
```

### インデックスの解釈

- すべてのインデックスは「入力トレース」のインデックス
- 出力トレースでは挿入によりインデックスがずれる
- 出力を検証する際は、このずれを考慮する必要がある

## 7. 検証方法

```bash
# 挿入実行
./trace_insert_range \
    --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out /tmp/inserted.trace \
    --src-begin 350820 --src-end 350920 \
    --insert-at 336480

# 検証1: 出力トレースの長さ
ls -l /tmp/inserted.trace
# 元: 201707970 × 64 = 12,909,310,080 bytes
# 期待: (201707970 + 100) × 64 = 12,909,316,480 bytes

# 検証2: 挿入位置の前後を確認
./trace_inspect --trace /tmp/inserted.trace --start 336478 --max 10
# idx=336478, 336479: 元のAアクセス
# idx=336480 ~ 336579: 挿入されたBアクセス
# idx=336580 ~: 元のAアクセス（続き）

# 検証3: 元のBチャンク位置（シフトしている）
./trace_inspect --trace /tmp/inserted.trace --start 350920 --max 5
# idx=350920 は元の350820に相当（100レコードシフト）
```

## 8. 実装優先度

1. **Phase 3.5: trace_insert_range（単一挿入）** ← 最初に実装
   - シンプルな1パス実装
   - 基本的な効果検証に十分

2. **Phase 4: trace_edit（複数挿入）** ← 必要になったら
   - 編集計画ファイル方式
   - 全outer iterationへの挿入などに対応

## 9. 実装例（擬似コード）

```c
// 1パス実装
int main() {
    // 引数パース
    // ...

    // Phase 1: src範囲をメモリに読み込み
    fseek(fp_in, src_begin * sizeof(input_instr), SEEK_SET);
    input_instr *src_buf = malloc((src_end - src_begin) * sizeof(input_instr));
    fread(src_buf, sizeof(input_instr), src_end - src_begin, fp_in);

    // Phase 2: 入力を先頭から処理
    fseek(fp_in, 0, SEEK_SET);
    int64_t idx = 0;
    int inserted = 0;
    input_instr rec;

    while (fread(&rec, sizeof(rec), 1, fp_in) == 1) {
        // 挿入ポイントに達したら挿入
        if (!inserted && idx == insert_at) {
            fwrite(src_buf, sizeof(input_instr), src_end - src_begin, fp_out);
            inserted = 1;
        }

        // 元レコードを出力
        fwrite(&rec, sizeof(input_instr), 1, fp_out);
        idx++;
    }

    free(src_buf);
    return 0;
}
```
