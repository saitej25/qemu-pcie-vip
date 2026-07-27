#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
QEMU=${QEMU:-$ROOT/qemu/build/qemu-system-x86_64}
SOCKET=${PCIE_VIP_SOCKET:-/tmp/pcie-vip.sock}
TIMEOUT_MS=${PCIE_VIP_TIMEOUT_MS:-5000}

if [ ! -x "$QEMU" ]; then
    echo "QEMU binary not found: $QEMU" >&2
    echo "Build qemu v11.0.3 first (see qemu/docs/specs/pcie-vip.rst)." >&2
    exit 1
fi
exec "$QEMU" -machine q35 \
    -device pcie-vip,socket="$SOCKET",timeout-ms="$TIMEOUT_MS" "$@"
