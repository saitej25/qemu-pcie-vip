#!/usr/bin/env bash
set -euo pipefail

# Pure-RTL Verilator path for the generic Alex TLP/AXI-Lite endpoint.
# This is intentionally independent of Questa's -sv_lib flow and Mini-ICS
# DPI: it is a deterministic smoke test for the same TLP boundary used by
# QEMU, and therefore runs on hosts without cocotb or a simulator license.
# Use the --cc/--exe flow because Ubuntu 22.04 ships Verilator 4.x, which does
# not provide the newer --binary or --timing convenience options.  The
# compatibility testbench has no simulator timing controls; the C++ harness
# drives its clock explicitly.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VERILOG_PCIE="${ROOT_DIR}/third_party/verilog-pcie/rtl"
BUILD_DIR="${VERILATOR_ALEX_BUILD_DIR:-${SCRIPT_DIR}/build/verilator}"

if ! command -v verilator >/dev/null 2>&1; then
    echo "run_verilator_alex.sh: verilator is not installed or not on PATH." >&2
    echo "Install it with your distribution package manager, then rerun:" >&2
    echo "  sudo apt-get install verilator" >&2
    exit 2
fi

if [[ ! -f "${VERILOG_PCIE}/pcie_axil_master_minimal.v" ]]; then
    echo "run_verilator_alex.sh: missing third_party/verilog-pcie submodule" >&2
    echo "Run: git submodule update --init --recursive" >&2
    exit 2
fi

mkdir -p "${BUILD_DIR}"
args=(
    --cc --exe --build
    -Wall -Wno-fatal -Wno-DECLFILENAME
    --top-module alex_verilator_compat_tb
    --Mdir "${BUILD_DIR}"
    "${VERILOG_PCIE}/pcie_axil_master_minimal.v"
    "${VERILOG_PCIE}/dma_if_pcie.v"
    "${VERILOG_PCIE}/dma_if_pcie_rd.v"
    "${VERILOG_PCIE}/dma_if_pcie_wr.v"
    "${SCRIPT_DIR}/axi_lite_slave_model.sv"
    "${SCRIPT_DIR}/pcie_vip_alex_endpoint.sv"
    "${SCRIPT_DIR}/pcie_vip_dma_if_pcie.sv"
    "${SCRIPT_DIR}/verilator_compat_tb.sv"
    "${SCRIPT_DIR}/verilator_main.cpp"
)

if [[ "${VERILATOR_TRACE:-0}" == 1 ]]; then
    args+=(--trace-fst)
fi

echo "run_verilator_alex.sh: building pure-RTL Alex endpoint test"
verilator "${args[@]}"

run_args=()
if [[ "${VERILATOR_TRACE:-0}" == 1 ]]; then
    run_args+=(+TRACE)
fi

echo "run_verilator_alex.sh: running ${BUILD_DIR}/Valex_verilator_compat_tb"
"${BUILD_DIR}/Valex_verilator_compat_tb" "${run_args[@]}"
