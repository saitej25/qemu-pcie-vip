#!/usr/bin/env bash
# Builds the DPI shared library, compiles the RTL with QuestaSim's
# vlog, then runs the testbench under vsim linked against the DPI .so,
# alongside the standalone mini_ics_host_client acting as the host
# side. Requires `vlog`/`vsim` (QuestaSim) on PATH.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${MINI_ICS_BUILD_DIR:-${ROOT_DIR}/build}"
SOCKET_PATH="${MINI_ICS_SOCKET_PATH:-/tmp/mini_ics_questa_$$.sock}"
SIM_WORK_DIR="${MINI_ICS_SIM_WORK_DIR:-${ROOT_DIR}/build/questa_work}"

if ! command -v vlog >/dev/null 2>&1 || ! command -v vsim >/dev/null 2>&1; then
  echo "run_questa.sh: vlog/vsim not found on PATH; QuestaSim is required for this script." >&2
  exit 2
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "${BUILD_DIR}" -j"$(nproc)" --target mini_ics_dpi mini_ics_host_client

DPI_LIB="${BUILD_DIR}/libmini_ics_dpi.so"
if [ ! -f "${DPI_LIB}" ]; then
  echo "run_questa.sh: ${DPI_LIB} not built. Is svdpi.h discoverable (see CMakeLists.txt)?" >&2
  exit 2
fi

rm -f "${SOCKET_PATH}"
mkdir -p "${SIM_WORK_DIR}"
cd "${SIM_WORK_DIR}"

rm -rf work
vlib work
vlog -sv \
  "${ROOT_DIR}/../third_party/verilog-pcie/rtl/pcie_axil_master_minimal.v" \
  "${ROOT_DIR}/../rtl/alex/axi_lite_slave_model.sv" \
  "${ROOT_DIR}/../rtl/alex/pcie_vip_alex_endpoint.sv" \
  "${ROOT_DIR}/dpi/mini_ics_dpi_pkg.sv" \
  "${ROOT_DIR}/rtl/nvme_register_model.sv" \
  "${ROOT_DIR}/rtl/mini_ics_endpoint_model.sv" \
  "${ROOT_DIR}/rtl/mini_ics_tb.sv"

# Start the host client in the background; it will block on connect()
# (bounded by its own --timeout-ms) until the testbench's mini_ics_init
# call brings the listener up, so ordering here is not racy.
"${BUILD_DIR}/mini_ics_host_client" --socket "${SOCKET_PATH}" --timeout-ms 10000 &
CLIENT_PID=$!

cleanup() {
  if kill -0 "${CLIENT_PID}" 2>/dev/null; then
    kill "${CLIENT_PID}" 2>/dev/null || true
    wait "${CLIENT_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

vsim -c -sv_lib "${BUILD_DIR}/libmini_ics_dpi" \
  -do "run -all; quit -f" \
  work.mini_ics_tb \
  +MINI_ICS_SOCKET="${SOCKET_PATH}"

wait "${CLIENT_PID}"
CLIENT_RC=$?

if [ "${CLIENT_RC}" -eq 0 ]; then
  echo "run_questa.sh: PASS"
  exit 0
else
  echo "run_questa.sh: FAIL (host client rc=${CLIENT_RC})"
  exit 1
fi
