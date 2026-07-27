#!/usr/bin/env bash
set -euo pipefail

# Verilator entry point.  The Alex endpoint has a pure SystemVerilog
# --binary test, so it does not depend on Questa's dynamically loaded DPI
# library.  The legacy Mini-ICS socket test remains a separate Questa flow;
# its compiled-C++ DPI binding is intentionally not hidden behind this name.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

exec "${ROOT_DIR}/rtl/alex/run_verilator_alex.sh" "$@"
