#!/usr/bin/env bash
# Build the 1.44MB floppy image (POSIX port of mkimg.ps1).
#
# Layout is hard-wired to boot/boot.asm:
#   sector 0   : boot.bin  (512 bytes, ends with 0xAA55)
#   sector 1-4 : setup.bin (SETUPLEN = 4)
#   sector 5+  : kernel.bin (read_it starts at sread = 1 + SETUPLEN)
#
# Padding to the full 1474560 bytes matters: read_it reads up to SYSSIZE (192KB)
# and its retry branch loops forever, so a short image traps the loader.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
OUT="$BUILD/mini-os.img"

BOOT="$BUILD/boot.bin"
SETUP="$BUILD/setup.bin"
KERNEL="$BUILD/kernel.bin"

for f in "$BOOT" "$SETUP" "$KERNEL"; do
    [ -f "$f" ] || { echo "missing $f - build first" >&2; exit 1; }
done

boot_sz=$(stat -c %s "$BOOT")
setup_sz=$(stat -c %s "$SETUP")
kernel_sz=$(stat -c %s "$KERNEL")

[ "$boot_sz" -eq 512 ] || { echo "boot.bin must be 512 bytes, got $boot_sz" >&2; exit 1; }
[ "$(od -An -tx1 -j510 -N2 "$BOOT" | tr -d ' \n')" = "55aa" ] || {
    echo "boot.bin has no 0xAA55 signature" >&2; exit 1; }
[ "$setup_sz" -le 2048 ] || { echo "setup.bin exceeds 4 sectors, got $setup_sz" >&2; exit 1; }
[ "$kernel_sz" -le 196608 ] || { echo "kernel.bin exceeds SYSSIZE=192KB, got $kernel_sz" >&2; exit 1; }

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1474560 count=1 status=none
dd if="$BOOT"   of="$OUT" bs=512 seek=0 conv=notrunc status=none
dd if="$SETUP"  of="$OUT" bs=512 seek=1 conv=notrunc status=none
dd if="$KERNEL" of="$OUT" bs=512 seek=5 conv=notrunc status=none

echo "image  : $OUT"
echo "  boot   : $boot_sz bytes @ 0x0000"
echo "  setup  : $setup_sz bytes @ 0x0200 (4 sectors reserved)"
echo "  kernel : $kernel_sz bytes @ 0x0A00"
echo "  total  : $(stat -c %s "$OUT") bytes"
