CC     = gcc
CFLAGS = -O3 -Wall -Wextra -std=c11

SRC = benchmark.c

# デフォルト: 両方ビルド
all: benchmark_dense benchmark_stride

# 密アクセス版 (ACCESS_MODE=0)
benchmark_dense: $(SRC)
	$(CC) $(CFLAGS) -DACCESS_MODE=0 -o $@ $<

# ストライド版 (ACCESS_MODE=1)
# STRIDE は要素数単位のストライド長として上書き可能:
#   make benchmark_stride STRIDE=16
STRIDE ?= 8

benchmark_stride: $(SRC)
	$(CC) $(CFLAGS) -DACCESS_MODE=1 -DSTRIDE_ELEMS=$(STRIDE) -o $@ $<

clean:
	rm -f benchmark_dense benchmark_stride
