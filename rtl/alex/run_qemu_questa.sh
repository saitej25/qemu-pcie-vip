#!/usr/bin/env bash
# Run the live QEMU -> Mini-ICS -> TLP -> Alex AXI-Lite path under Questa.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MINI_DIR="${ROOT_DIR}/mini-ics"
BUILD_DIR="${MINI_DIR}/build"
WORK_DIR="${SCRIPT_DIR}/build/qemu_questa"
SOCKET_PATH="${PCIE_VIP_SOCKET:-/tmp/pcie-vip-alex-questa.sock}"
WLF_PATH="${PCIE_VIP_WLF:-/tmp/pcie-vip-alex-questa.wlf}"

cmake -S "${MINI_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "${BUILD_DIR}" -j"$(nproc)" --target mini_ics_dpi >/dev/null

rm -f "${SOCKET_PATH}" "${WLF_PATH}"
mkdir -p "${WORK_DIR}"
cd "${WORK_DIR}"

rm -rf work
vlib work
vlog -sv \
  "${MINI_DIR}/dpi/mini_ics_dpi_pkg.sv" \
  "${ROOT_DIR}/third_party/verilog-pcie/rtl/pcie_axil_master_minimal.v" \
  "${ROOT_DIR}/third_party/verilog-pcie/rtl/dma_if_pcie.v" \
  "${ROOT_DIR}/third_party/verilog-pcie/rtl/dma_if_pcie_rd.v" \
  "${ROOT_DIR}/third_party/verilog-pcie/rtl/dma_if_pcie_wr.v" \
  "${SCRIPT_DIR}/axi_lite_slave_model.sv" \
  "${SCRIPT_DIR}/pcie_vip_alex_endpoint.sv" \
  "${SCRIPT_DIR}/pcie_vip_dma_if_pcie.sv" \
  "${SCRIPT_DIR}/mini_ics_alex_tb.sv"

vsim -c -voptargs="+acc" \
  -sv_lib "${BUILD_DIR}/libmini_ics_dpi" \
  -wlf "${WLF_PATH}" \
  work.mini_ics_alex_tb "+MINI_ICS_SOCKET=${SOCKET_PATH}" \
  -do 'add wave -r sim:/mini_ics_alex_tb/dut/*; add wave -r sim:/mini_ics_alex_tb/dut/axil_master_inst/*; add wave -r sim:/mini_ics_alex_tb/dut/axil_slave_model/*; when -label qemu_pass {sim:/mini_ics_alex_tb/sim_pass == 1} {echo "QEMU ALEX AXI PASS"; stop}; run -all; quit -f'
