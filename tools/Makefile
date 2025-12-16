# Makefile for trace surgery tools

CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -std=c99

TOOLS = trace_inspect

.PHONY: all clean

all: $(TOOLS)

trace_inspect: trace_inspect.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TOOLS)
