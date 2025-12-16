# Makefile for trace surgery tools

CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -std=c99

TOOLS = trace_inspect find_b_accesses

.PHONY: all clean

all: $(TOOLS)

trace_inspect: trace_inspect.c
	$(CC) $(CFLAGS) -o $@ $<

find_b_accesses: find_b_accesses.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TOOLS)
