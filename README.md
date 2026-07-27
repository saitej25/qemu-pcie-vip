# QEMU PCIe VIP

Standalone PCIe verification environment combining a QEMU `pcie-vip` device,
Mini-ICS v2 socket transport, Linux guest tools/driver, and Alex Forencich's
`verilog-pcie` components.

## Current live path

```text
Linux pcimem / driver
  -> QEMU pcie-vip BAR0
  -> Mini-ICS v2 Unix socket
  -> Questa SystemVerilog TLP BFM
  -> Alex pcie_axil_master_minimal
  -> AXI-Lite register slave model
```

The QEMU guest sees PCI ID `1234:11e9`, a 64-KiB BAR0, and four MSI-X vectors.
The deterministic software DMA backend remains available as a regression
baseline. The Alex `dma_if_pcie` descriptor/RAM path is the next integration
stage; the current bring-up path already drives QEMU MMIO through Alex
AXI-Lite and captures it in Questa waves.

## Checkout

```sh
git clone git@github.com:saitej25/qemu-pcie-vip.git
cd qemu-pcie-vip
git submodule update --init --recursive
```

The pinned submodules are QEMU, Alex `verilog-pcie`, Alex `cocotbext-pcie`,
Buildroot, and the upstream `billfarrow/pcimem` BAR utility.

The QEMU integration is maintained as a small patch against the pinned QEMU
submodule:

```sh
git -C qemu checkout v11.0.3
git -C qemu apply ../patches/qemu-pcie-vip.patch
```

## Questa QEMU/MMIO run

Build QEMU and the Mini-ICS DPI library, then start the RTL listener:

```sh
./rtl/alex/run_qemu_questa.sh
```

In another terminal launch QEMU with:

```sh
cd qemu/build
./qemu-system-x86_64 \
  -machine q35 -m 2G -snapshot -nographic -serial mon:stdio \
  -drive file=./guest.qcow2,if=virtio,format=qcow2 \
  -drive file=./seed.img,if=virtio,format=raw \
  -netdev user,id=n0,hostfwd=tcp::2224-:22 \
  -device virtio-net-pci,netdev=n0 \
  -device 'pcie-vip,socket=/tmp/pcie-vip-alex-questa.sock,timeout-ms=5000' \
  -D /tmp/qemu-vip-alex.log
```

Inside the guest:

```sh
sudo /usr/local/bin/pcie-vip-pcimem 0000:00:03.0 read 0x0 32
sudo /usr/local/bin/pcie-vip-pcimem 0000:00:03.0 write 0x14 32 0xdeadbeef
sudo /usr/local/bin/pcie-vip-pcimem 0000:00:03.0 read 0x14 32
sudo /usr/local/bin/pcie-vip-run
```

The Questa waveform is `/tmp/pcie-vip-alex-questa.wlf`. Important wave
hierarchies are `sim:/mini_ics_alex_tb/dut/axil_master_inst/*`,
`sim:/mini_ics_alex_tb/dut/axil_slave_model/*`, and the TLP signals under
`sim:/mini_ics_alex_tb/dut/*`.

## Verilator Alex test

The same generic TLP-to-AXI-Lite endpoint has a pure-RTL Verilator runner.
It drives CAP/CC/CSTS reads, a `0xdeadbeef` CSR write/readback, and the SQ
doorbell without Mini-ICS DPI or a Questa license:

```sh
sudo apt-get install verilator       # once, if needed
make -C rtl/alex verilator_alex
# or: mini-ics/scripts/run_verilator.sh
```

The expected result is `VERILATOR ALEX AXI/TLP PASS`. Set
`VERILATOR_TRACE=1` to emit `rtl/alex/alex_verilator.fst`. This is the
Verilator RTL milestone; the live QEMU socket remains the Questa flow until
a compiled-C++ Mini-ICS DPI binding is supplied for Verilator.

## Deterministic baseline

The software backend is still available for fast tests:

```sh
mini-ics/build/mini_ics_server --demo --persistent \
  --socket /tmp/pcie-vip.sock --log-level info
```

Do not run the deterministic listener and the Questa listener on the same
socket. QEMU does not reconnect after a runtime backend disconnect; restart
QEMU when changing listeners.

## Scope

QEMU validates guest-visible PCI enumeration, BAR access, DMA mapping, and
MSI-X delivery. It does not model PHY, LTSSM, DLLP, lane training, or
electrical behavior. Use `cocotbext-pcie` for deeper protocol-layer tests.
