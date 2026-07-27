CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic

.PHONY: all clean pcimem

PCIMEM_SRC ?= ../../../third_party/pcimem/pcimem.c

all: pcie-vip-pcimem pcie-vip-run pcimem

pcie-vip-pcimem: pcie-vip-pcimem.c
	$(CC) $(CFLAGS) -o $@ $<

pcie-vip-run: pcie-vip-run.c
	$(CC) $(CFLAGS) -o $@ $<

pcimem: $(PCIMEM_SRC)
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 -o $@ $<

clean:
	rm -f pcie-vip-pcimem pcie-vip-run
