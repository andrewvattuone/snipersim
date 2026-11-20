# Builds the native sharing microbenchmark (no Sniper markers needed).
CC ?= gcc
CFLAGS ?= -O1 -Wall

all: sharetest coherence_regress

# -no-pie keeps the arrays at fixed virtual addresses, so a line can be traced
# with SNIPER_TRACE_VADDR (address from nm)
sharetest: sharetest.c
	$(CC) $(CFLAGS) -no-pie -pthread -o $@ $<

coherence_regress: coherence_regress.c
	$(CC) $(CFLAGS) -no-pie -pthread -o $@ $<

clean:
	rm -f sharetest coherence_regress

.PHONY: all clean
