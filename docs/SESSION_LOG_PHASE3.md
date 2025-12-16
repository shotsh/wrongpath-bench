# Session Log: Phase 3 Implementation (trace_overwrite_range)

日付: 2025-12-16

## 完了したタスク

### Phase 3: trace_overwrite_range.c の実装

#### 1. 新規作成ファイル
- `tools/trace_overwrite_range.c` - トレース範囲上書きツール

#### 2. 更新したファイル
- `tools/trace_inspect.c` - `--start IDX` オプション追加
- `tools/Makefile` - `trace_overwrite_range` をビルドターゲットに追加
- `tools/README.md` - Phase 3 ドキュメント追加

---

## trace_overwrite_range.c の仕様

### CLI オプション
```
./trace_overwrite_range --in PATH --out PATH --src-begin I --src-end J --dst-begin K [--dry-run]
```

| オプション | 説明 |
|-----------|------|
| `--in PATH` | 入力トレースファイル (必須) |
| `--out PATH` | 出力トレースファイル (必須、`--dry-run` 時は不要) |
| `--src-begin I` | コピー元の開始インデックス (含む、必須) |
| `--src-end J` | コピー元の終了インデックス (含まない、必須) |
| `--dst-begin K` | コピー先の開始インデックス (必須) |
| `--dry-run` | 範囲検証のみ、出力ファイルを作成しない |

### 動作
1. トレースを先頭から順に読み込む
2. `idx` が `[dst_begin, dst_begin + (src_end - src_begin))` 範囲内の場合:
   - 元のレコードを破棄し、`[src_begin, src_end)` のレコードを順に出力
3. それ以外の場合:
   - 元のレコードをそのまま出力
4. 出力トレースの総レコード数は入力と同じ（上書きモード）

---

## trace_inspect.c の拡張

### 追加オプション
```
--start IDX    開始インデックス (デフォルト: 0)
```

### 使用例
```bash
# idx=350820 から10レコードを表示
./trace_inspect --trace ../wp_A64KB_B64MB_chunk32KB_stride16_os2 --start 350820 --max 10
```

---

## 検証結果

### テストコマンド
```bash
# dry-run テスト
./trace_overwrite_range --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --src-begin 350820 --src-end 350920 --dst-begin 322141 --dry-run

# 実際の上書きテスト
./trace_overwrite_range --in ../wp_A64KB_B64MB_chunk32KB_stride16_os2 \
    --out /tmp/test_overwrite.trace \
    --src-begin 350820 --src-end 350920 --dst-begin 322141
```

### 検証出力
```
元トレースのsrc範囲 (Bアクセス):
idx=350820 ip=0x4008b3 src_mem=[0xc33fd010] dst_mem=[]

元トレースのdst範囲 (Aアクセス):
idx=322141 ip=0x400880 src_mem=[0xfc62a0] dst_mem=[]

overwrite後のdst範囲 (Bアクセスに上書き):
idx=322141 ip=0x4008b3 src_mem=[0xc33fd010] dst_mem=[]
```

**結果:** Aアクセス (IP=0x400880) が Bアクセス (IP=0x4008b3) で正しく上書きされた。

---

## フェーズ1-3 完了サマリー

| フェーズ | ツール | 状態 |
|---------|--------|------|
| Phase 1 | trace_inspect | ✅ 完了 (sanity check, --start 追加) |
| Phase 2 | find_b_accesses | ✅ 完了 (offset カラム追加) |
| Phase 3 | trace_overwrite_range | ✅ 完了 |

---

## 次のステップ候補

1. **ChampSim での評価**
   - 元トレースと改造トレースの IPC/MPKI 比較
   - warmup/sim 区間の調整

2. **将来拡張 (SPEC.md セクション4)**
   - 挿入モード（長さ変更）の実装
   - trace_slice ツール
   - ループ自動検出

---

## 関連ファイル

- `docs/TRACE_SURGERY_CONTEXT.md` - 背景と狙い
- `docs/SPEC.md` - 仕様書
- `tools/README.md` - ツール使用方法
