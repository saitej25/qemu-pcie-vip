# Alex `verilog-pcie` integration

This directory contains the transaction-level cocotb adapter used to connect
the QEMU `pcie-vip` BAR endpoint to Alex Forencich's pinned `verilog-pcie`
components. QEMU remains the PCI configuration/MSI-X owner. The adapter maps
Mini-ICS v2 MMIO messages to `pcie_axil_master` traffic and maps endpoint DMA
TLPs back to Mini-ICS DMA requests.

Dependencies are kept outside this repository in `third_party/verilog-pcie`
and `third_party/cocotbext-pcie`. The bridge is simulator-neutral Python and
can be imported by both Verilator and Questa cocotb runners.

Example runner setup:

```sh
PYTHONPATH=rtl/alex:third_party/cocotbext-pcie \
  python3 -m pytest rtl/alex/test_bridge.py
```

For a QEMU connection, the adapter must be the Unix-socket listener (QEMU
connects to it):

```sh
PYTHONPATH=rtl/alex python3 rtl/alex/run_bridge.py /tmp/pcie-vip.sock
```

The reference callbacks in `run_bridge.py` are deliberately inert; the Alex
cocotb wrapper replaces them with AXI-Lite/TLP callbacks. This keeps socket
framing testable independently from simulator scheduling.

`cocotb_tlp_adapter.py` defines the callback contract used by the real DUT
driver: AXI-Lite BAR reads/writes, endpoint DMA reads/writes, and MSI-X
delivery. The simulator-specific coroutine implementation belongs in the
Questa/Verilator test harness, not in the Mini-ICS codec.

The electrical PHY, LTSSM, DLLP, and lane-training layers are intentionally
not represented here; use `cocotbext-pcie` standalone tests for those layers.

The dedicated signal-level top is `alex_endpoint_tb.sv`. Compile the Alex
wrapper independently of the legacy Mini-ICS model with:

```sh
cd rtl/alex
make questa_compile
```

The endpoint now terminates its AXI-Lite master in
`axi_lite_slave_model.sv`.  The intended direction is QEMU guest MMIO as the
transaction-level master, followed by the Mini-ICS/TLP adapter, Alex's
`pcie_axil_master_minimal`, and finally this AXI-Lite slave.  The AXI signals
(`axil_aw*`, `axil_w*`, `axil_b*`, `axil_ar*`, and `axil_r*`) are kept as named
hierarchical nets for waveform inspection.  The QEMU live socket still uses
the transaction-level endpoint until the cocotb socket-to-TLP driver is
enabled.  For Questa bring-up, `mini_ics_alex_tb.sv` provides a pure
SystemVerilog Mini-ICS-to-TLP BFM, so QEMU BAR accesses can be driven through
`pcie_axil_master_minimal` and observed at the AXI-Lite slave:

```sh
./rtl/alex/run_qemu_questa.sh
```

The bring-up top keeps the deterministic DMA sequence enabled while the
`dma_if_pcie` descriptor/RAM path is being connected; MMIO is now
QEMU-to-TLP-to-Alex-AXI-Lite.

## Full QEMU + Verilator host path

`run_qemu_verilator.sh` provides a live host-side path without Questa DPI.
Its C++ harness owns `MiniIcsServer`, converts Mini-ICS MMIO messages into
the wrapper's TLP BFM inputs, and runs the Alex AXI-Lite endpoint under
Verilator 4.x or newer. Start it before QEMU:

```sh
make -C rtl/alex qemu_verilator
```

In another terminal launch QEMU with:

```sh
-device 'pcie-vip,socket=/tmp/pcie-vip-verilator.sock,timeout-ms=5000'
```

BAR reads and writes therefore travel QEMU → Mini-ICS → TLP BFM → Alex
`pcie_axil_master_minimal` → AXI-Lite register block. The guest regression
continues to use the deterministic DMA/MSI-X callback; connecting
`dma_if_pcie` and its descriptor RAM is the next DMA milestone.

The generic descriptor control plane is also present at BAR0 offsets
`0x1040`–`0x105c`: descriptor/completion base addresses, ring count and
tail, status, and control. Descriptors are 32-byte little-endian records;
completion records are 16 bytes. The Verilator Mini-ICS harness services
those records with QEMU's DMA requests and raises MSI-X vector 0 for an
interrupt-flagged descriptor.

## Verilator

The pure-RTL Alex path has a simulator-independent Verilator runner.  It
uses the portable `--cc --exe --build` flow supported by Verilator 4.x and
does not load the Questa DPI library or require cocotb.  The compatibility
testbench has no simulator timing controls; its C++ harness drives each clock
edge explicitly:
one-segment Memory Read/Write TLPs into `pcie_axil_master_minimal` and checks
the AXI-Lite register block, including CC/CSTS and the SQ doorbell.

```sh
git submodule update --init --recursive
sudo apt-get install verilator       # once, if it is not installed
make -C rtl/alex verilator_alex
```

For an FST waveform, set `VERILATOR_TRACE=1`; the runner writes
`rtl/alex/alex_verilator.fst`.  This is the RTL-level Verilator milestone.
The live QEMU socket path remains the Questa path until a compiled-C++
Mini-ICS DPI binding is added; Verilator cannot consume Questa's `-sv_lib`
shared library directly.  For the host-side Verilator adapter, use the
`run_qemu_verilator.sh` path above.

For a host-only sandbox check (no Linux guest image required), build the
adapter and run the orchestrator against a patched QEMU binary:

```sh
QEMU_VERILATOR_BUILD_ONLY=1 make -C rtl/alex qemu_verilator
python3 scripts/ci/qemu_verilator_sandbox.py \
  qemu/build/qemu-system-x86_64 \
  rtl/alex/build/qemu_verilator/Valex_qemu_verilator_top
```

The sandbox starts and cleans up the adapter, drives QEMU through QMP, and
checks that PCI ID `1234:11e9` enumerates. Its adapter log is written to
`/tmp/pcie-vip-verilator-sandbox.log`.
