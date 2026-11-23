#!/bin/bash
# 抓屏：无头 QEMU + gdb 断在 kernel_idle（自检结束的空闲入口），dump VGA 文本区并打印成 24 行（带列宽）
set -u
cd "$HOME/mini-os" || exit 1
killall -q -9 qemu-system-i386 2>/dev/null
sleep 1
ADDR=$(nm -n build/kernel.elf | awk '$3=="kernel_idle"{print "0x"$1; exit}')
[ -n "$ADDR" ] || ADDR=$(nm -n build/kernel.elf | awk '$3=="kernel_halt"{print "0x"$1; exit}')
[ -n "$ADDR" ] || { echo "NO-ADDR"; exit 1; }
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none -gdb tcp::1234 -S >/tmp/q.log 2>&1 &)
sleep 1
gdb -batch -q -ex 'set architecture i386' -ex 'set confirm off' -ex 'file build/kernel.elf' \
  -ex "target remote 127.0.0.1:1234" -ex "break *$ADDR" -ex 'continue' \
  -ex 'dump binary memory /tmp/vga.bin 0xb8000 0xb8fa0' -ex 'detach' >/tmp/gdb.log 2>&1
python3 tools/vga-screen.py /tmp/vga.bin | awk '{print length($0)": "$0}'
killall -q -9 qemu-system-i386 2>/dev/null
