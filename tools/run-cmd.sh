#!/bin/bash
# 一条命令跑一次：起 qemu（带串口文件）、敲一串键、等、抓屏 + 打印串口尾部。
# 用法： tools/run-cmd.sh <秒> <键...>   例： tools/run-cmd.sh 9 e x e c spc u p r o g 3 ret
# 目的：把"起机器 / 敲键 / 抓屏 / 读串口"固定成一条，避免每次现场手拼命令行而漏步骤。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
WAIT="$1"; shift
QMP=/tmp/qmp.sock
SCR=/tmp/rc-screen.bin
SER=/tmp/rc-serial.log
IMG=/tmp/rc.img

killall -q -9 qemu-system-i386 2>/dev/null
sleep 1
rm -f $QMP $SCR $SER
python3 tools/datadisk.py $IMG build/uprog.bin '' build/uprog.elf '' '' '' build/uprog3.bin \
  >/tmp/rc-datadisk.log 2>&1 || { echo "造盘失败"; exit 1; }
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER -qmp unix:$QMP,server,nowait >/tmp/rc-qemu.log 2>&1 &)
sleep 3
if [ "$#" -gt 0 ]; then
  python3 tools/qemu-sendkey.py $QMP "$@" >/dev/null 2>&1
fi
sleep "$WAIT"
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $SCR" >/dev/null 2>&1
echo "=== screen ==="
python3 tools/vga-screen.py $SCR | head -12
echo "=== serial tail ==="
tail -"${SERIAL_LINES:-25}" $SER
killall -q -9 qemu-system-i386 2>/dev/null
