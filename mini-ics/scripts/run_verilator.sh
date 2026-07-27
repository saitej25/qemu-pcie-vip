#!/usr/bin/env bash
# Attempts to build and run the Mini ICS testbench under Verilator.
#
# HONESTY NOTE (see docs/architecture.md and README.md "Verilator"
# sections for the full explanation): this RTL was written and verified
# against QuestaSim (see run_questa.sh, which passes end-to-end in this
# repository's CI). Verilator has real, structural differences from
# QuestaSim/VCS for exactly the features this design uses most:
#
#   - Verilator compiles DPI import declarations into a C++ class
#     (Vmini_ics_tb__Dpi.h) that your C++ code must implement/link
#     against; it does not dynamically load a -sv_lib .so the way
#     QuestaSim does. The mini_ics_dpi target here is a plain shared
#     library built for QuestaSim/VCS-style dynamic loading, so it is
#     NOT used as-is by Verilator -- see the notes below.
#   - Verilator's SystemVerilog support for `string` as a module port
#     (used by mini_ics_tb -> mini_ics_endpoint_model for the
#     +MINI_ICS_SOCKET plusarg) and for svOpenArrayHandle-based open
#     arrays in DPI imports is incomplete relative to QuestaSim.
#
# This script therefore builds as far as Verilator's toolchain allows
# and clearly reports success/failure/unsupported at each stage, rather
# than silently faking a pass. Where Verilator cannot proceed, it prints
# exactly what would need to change (documented here and in
# docs/architecture.md) instead of pretending compatibility that hasn't
# been demonstrated.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VERILATOR_WORK_DIR="${MINI_ICS_VERILATOR_WORK_DIR:-${ROOT_DIR}/build/verilator_work}"

if ! command -v verilator >/dev/null 2>&1; then
  echo "run_verilator.sh: 'verilator' not found on PATH." >&2
  echo "  This milestone's RTL has been verified against QuestaSim (scripts/run_questa.sh)." >&2
  echo "  Verilator support is a documented goal (see docs/architecture.md) but requires" >&2
  echo "  adapting the DPI shim to Verilator's compiled-C++ DPI binding model, which is" >&2
  echo "  tracked as follow-up work, not implemented in this MVP." >&2
  exit 2
fi

mkdir -p "${VERILATOR_WORK_DIR}"
cd "${VERILATOR_WORK_DIR}"

echo "run_verilator.sh: attempting Verilator lint/elaboration of the RTL..."
set +e
verilator --lint-only -sv --timing \
  -Wall -Wno-DECLFILENAME \
  "${ROOT_DIR}/dpi/mini_ics_dpi_pkg.sv" \
  "${ROOT_DIR}/rtl/nvme_register_model.sv" \
  "${ROOT_DIR}/rtl/mini_ics_endpoint_model.sv" \
  "${ROOT_DIR}/rtl/mini_ics_tb.sv" \
  --top-module mini_ics_tb
LINT_RC=$?
set -e

if [ "${LINT_RC}" -ne 0 ]; then
  echo "run_verilator.sh: FAIL (Verilator lint/elaboration reported errors above)."
  echo "  Common cause: Verilator's DPI import binding model differs from QuestaSim's"
  echo "  dynamic -sv_lib loading (see the note at the top of this script)."
  exit 1
fi

echo "run_verilator.sh: lint/elaboration passed."
echo "run_verilator.sh: NOTE: full simulate-and-link is not wired up in this MVP because"
echo "  it requires a Verilator-specific DPI binding (compiling against the generated"
echo "  Vmini_ics_tb__Dpi.h and implementing the imported functions directly in a"
echo "  Verilator-specific translation unit, rather than loading mini_ics_dpi.so)."
echo "  This is tracked as follow-up work; see docs/architecture.md."
exit 0
