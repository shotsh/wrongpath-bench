# Makefile
CC     = gcc
CFLAGS = -O2 -Wall -Wextra

.PHONY: all clean

all: benchmark benchmark_trace

benchmark: benchmark.c
	$(CC) $(CFLAGS) benchmark.c -o benchmark

benchmark_trace: benchmark.c
	$(CC) $(CFLAGS) -DTRACE_MODE benchmark.c -o benchmark_trace

clean:
	rm -f benchmark benchmark_trace
