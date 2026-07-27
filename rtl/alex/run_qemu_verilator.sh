#!/usr/bin/env bash
set -euo pipefail

# Build and run the host-side QEMU -> Mini-ICS -> Verilator -> Alex AXI-Lite path.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VERILOG_PCIE="${ROOT_DIR}/third_party/verilog-pcie/rtl"
BUILD_DIR="${VERILATOR_QEMU_BUILD_DIR:-${SCRIPT_DIR}/build/qemu_verilator}"

command -v verilator >/dev/null 2>&1 || { echo "verilator is not installed" >&2; exit 2; }
mkdir -p "${BUILD_DIR}"
verilator_args=(--cc --exe --build -Wall -Wno-fatal -Wno-DECLFILENAME \
  --top-module alex_qemu_verilator_top --Mdir "${BUILD_DIR}" \
  "${VERILOG_PCIE}/pcie_axil_master_minimal.v" \
  "${SCRIPT_DIR}/axi_lite_slave_model.sv" \
  "${SCRIPT_DIR}/pcie_vip_alex_endpoint.sv" \
  "${SCRIPT_DIR}/pcie_vip_dma_if_pcie.sv" \
  "${VERILOG_PCIE}/dma_if_pcie.v" \
  "${VERILOG_PCIE}/dma_if_pcie_rd.v" \
  "${VERILOG_PCIE}/dma_if_pcie_wr.v" \
  "${SCRIPT_DIR}/alex_qemu_verilator_top.sv" \
  "${SCRIPT_DIR}/verilator_qemu_main.cpp" \
  "${ROOT_DIR}/mini-ics/src/protocol.cpp" "${ROOT_DIR}/mini-ics/src/socket.cpp" \
  "${ROOT_DIR}/mini-ics/src/transaction.cpp" "${ROOT_DIR}/mini-ics/src/logger.cpp" \
  "${ROOT_DIR}/mini-ics/src/server.cpp" \
  -CFLAGS "-I${ROOT_DIR}/mini-ics/include" -LDFLAGS "-pthread")

if [[ "${VERILATOR_TRACE:-0}" == 1 ]]; then
  # --trace is supported by Verilator 4.x and produces a portable VCD.
  verilator_args+=(--trace)
fi

verilator "${verilator_args[@]}"

if [[ "${QEMU_VERILATOR_BUILD_ONLY:-0}" == 1 ]]; then
  exit 0
fi

SOCKET_PATH="${PCIE_VIP_SOCKET:-/tmp/pcie-vip-verilator.sock}"
rm -f "${SOCKET_PATH}"
exec "${BUILD_DIR}/Valex_qemu_verilator_top" "${SOCKET_PATH}"
