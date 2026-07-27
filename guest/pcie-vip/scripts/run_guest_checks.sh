#!/bin/sh
set -eu

BDF=${1:-0000:01:00.0}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

command -v lspci >/dev/null 2>&1 || {
    echo "lspci is required in the guest" >&2
    exit 1
}

lspci -nn -s "$BDF"
lspci -vv -s "$BDF"
lspci -xxxx -s "$BDF"
"$ROOT/user/pcie-vip-pcimem" "$BDF" read 0x0 64
"$ROOT/user/pcie-vip-pcimem" "$BDF" write 0x100 32 0x12345678
"$ROOT/user/pcie-vip-pcimem" "$BDF" read 0x100 32
