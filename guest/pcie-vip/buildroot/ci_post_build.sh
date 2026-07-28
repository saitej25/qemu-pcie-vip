#!/bin/sh
set -eu

install -D -m 0755 /dev/stdin "$TARGET_DIR/etc/init.d/S99pcie-vip-ci" <<'EOF'
#!/bin/sh
set -eu

case "${1:-start}" in
start)
    bdf=$(lspci -Dnn | sed -n 's/^\([^ ]*\).*1234:11e9.*/\1/p' | head -n 1)
    test -n "$bdf"
    test "$(pcie-vip-pcimem "$bdf" read 0x0 32)" = 0x00000001
    pcie-vip-pcimem "$bdf" write 0x14 32 0xdeadbeef
    test "$(pcie-vip-pcimem "$bdf" read 0x14 32)" = 0xdeadbeef
    echo "PCIE-VIP GUEST PCIMEM PASS"
    ko=$(find /lib/modules -name pcie_vip.ko | head -n 1)
    test -n "$ko"
    insmod "$ko"
    pcie-vip-run
    poweroff -f
    ;;
esac
EOF
