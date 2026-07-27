CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic

.PHONY: all clean

all: pcie-vip-pcimem pcie-vip-run

pcie-vip-pcimem: pcie-vip-pcimem.c
	$(CC) $(CFLAGS) -o $@ $<

pcie-vip-run: pcie-vip-run.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f pcie-vip-pcimem pcie-vip-run
