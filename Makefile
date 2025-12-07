# Makefile for wrongpath-bench

CC      ?= gcc

# 共通フラグ
CFLAGS_COMMON ?= -O3 -march=native -Wall

# 実機 perf 用:
#   - BENCH_VERBOSE を定義して、# Params や sum の printf を有効化
CFLAGS_PERF   ?= $(CFLAGS_COMMON) -DBENCH_VERBOSE

# ChampSim トレース用:
#   - TRACE_MODE を定義
#   - BENCH_VERBOSE は付けないので、ほぼ無口なバイナリになる
CFLAGS_TRACE  ?= $(CFLAGS_COMMON) -DTRACE_MODE

SRC     = benchmark.c

.PHONY: all perf trace clean

# デフォルトは両方ビルド
all: benchmark benchmark_trace

# 実機 perf 用バイナリ
perf: benchmark

benchmark: $(SRC)
	$(CC) $(CFLAGS_PERF) -o $@ $<

# ChampSim トレース用バイナリ (TRACE_MODE 有効, B の init カット)
trace: benchmark_trace

benchmark_trace: $(SRC)
	$(CC) $(CFLAGS_TRACE) -o $@ $<

clean:
	rm -f benchmark benchmark_trace
