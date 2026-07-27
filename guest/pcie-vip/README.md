# Standalone QEMU PCIe VIP guest tools

This directory contains the Linux-side tools for the standalone `pcie-vip`
QEMU device.  It is independent of MemAI.

## Build the host utilities and driver

```sh
make -C user
make -C driver KDIR=/lib/modules/$(uname -r)/build
```

The upstream `billfarrow/pcimem` utility is pinned as the
`third_party/pcimem` submodule. Build and install it with:

```sh
make -C user pcimem
sudo install -m 0755 user/pcimem /usr/local/bin/pcimem
```

It uses the upstream sysfs-resource interface rather than the custom
BDF-based wrapper:

```sh
sudo pcimem /sys/bus/pci/devices/0000:00:03.0/resource0 0x14 w
sudo pcimem /sys/bus/pci/devices/0000:00:03.0/resource0 0x14 w 0xdeadbeef
```

The driver targets PCI ID `1234:11e9` and creates `/dev/pcie_vip0`.  Its
`PCIE_VIP_IOC_RUN` ioctl programs coherent command and completion buffers,
rings the BAR0 doorbell, waits for MSI-X, and checks the deterministic
completion pattern.

## Guest checks

Buildroot `2025.02.16` is pinned in `../../third_party/buildroot`. Configure
it with this external tree to produce a reproducible image containing
`pciutils` and both utilities:

```sh
make -C ../../third_party/buildroot BR2_EXTERNAL=$PWD/buildroot menuconfig
make -C ../../third_party/buildroot BR2_EXTERNAL=$PWD/buildroot
```

Enable **Target packages → PCIe VIP guest tools** in the generated
configuration. Build the out-of-tree kernel module separately with the guest
kernel headers (`make -C driver KDIR=...`), then copy `pcie_vip.ko` into the
image or load it from the guest filesystem.

With the device attached to a Q35 guest and the Mini ICS software endpoint
listening on `/tmp/pcie-vip.sock`:

```sh
lspci -nn
lspci -vv -s 01:00.0
lspci -xxxx -s 01:00.0
./pcie-vip-pcimem 0000:01:00.0 read 0x0 64
./pcie-vip-pcimem 0000:01:00.0 write 0x100 32 0x12345678
./pcie-vip-pcimem 0000:01:00.0 read 0x100 32
insmod pcie_vip.ko
./pcie-vip-run
```

Raw BAR access should be performed while `pcie_vip` is not bound.  The DMA
and MSI-X test must use the driver because it needs a kernel-owned DMA
address and an interrupt handler.

The GitHub Actions `qemu-sandbox` job builds the pinned Buildroot guest,
boots it with the Alex Verilator adapter, and runs the raw `pcie-vip-pcimem`
read/write/read sequence automatically.  The out-of-tree DMA driver remains
an additional guest test because it requires matching guest kernel headers.

## Protocol boundary

The guest sees ordinary PCI configuration space and BARs.  QEMU translates
BAR accesses and endpoint-originated DMA/MSI-X events to Mini ICS version 2.
QEMU does not emulate a physical PCIe link or replace Alex Forencich's RTL
simulation models.
