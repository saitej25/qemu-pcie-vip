#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
make -C "$ROOT/user"
make -C "$ROOT/driver" KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"
printf '%s\n' "Built guest tools and pcie_vip.ko"
