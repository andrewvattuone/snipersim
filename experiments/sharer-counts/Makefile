# Builds the native sharing microbenchmark (no Sniper markers needed).
CC ?= gcc
CFLAGS ?= -O1 -Wall

sharetest: sharetest.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f sharetest

.PHONY: clean
