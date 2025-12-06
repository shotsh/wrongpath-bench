# Makefile for wrongpath-bench

CC      ?= gcc
CFLAGS  ?= -O3 -march=native -Wall
SRC     = benchmark.c

.PHONY: all perf trace clean

# デフォルトは両方ビルド
all: benchmark benchmark_trace

# 実機 perf 用バイナリ
perf: benchmark

benchmark: $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

# ChampSim トレース用バイナリ (TRACE_MODE 有効, B の init カット)
trace: benchmark_trace

benchmark_trace: $(SRC)
	$(CC) $(CFLAGS) -DTRACE_MODE -o $@ $<

clean:
	rm -f benchmark benchmark_trace
