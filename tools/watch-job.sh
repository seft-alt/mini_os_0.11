#!/bin/bash
# 用 gdb 的观察点抓“谁把 job.magic 清零了”。
# 顺序：先起 qemu（不带 -S，自由跑）→ 等启动完 → 挂 gdb 设观察点 → 再敲键。
# 观察点地址由 nm 现取，不写死；每命中一次打一行现场与 bt。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
IMG=/tmp/gd.img
GLOG=/tmp/gdb-job.log
SLOG=/tmp/gd-serial.log

killall -q -9 qemu-system-i386 2>/dev/null
pkill -f "gdb -q" 2>/dev/null
sleep 1
rm -f $QMP $IMG $GLOG $SLOG
python3 tools/datadisk.py $IMG build/uprog.bin '' build/uprog.elf '' '' '' build/uprog3.bin \
  >/dev/null 2>&1 || { echo "造盘失败"; exit 1; }

job=$(nm -n build/kernel.elf | awk '$3=="job"{print "0x"$1; exit}')
[ -n "$job" ] || { echo "没找到 job 符号"; exit 1; }
magic=$(printf '0x%x' $((job + 20)))
echo "job=$job magic=$magic"

(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SLOG -qmp unix:$QMP,server,nowait \
   -gdb tcp::1234 >/tmp/gd-qemu.log 2>&1 &)
sleep 4

{
  echo "set architecture i386"
  echo "set pagination off"
  echo "set confirm off"
  echo "target remote :1234"
  echo "watch *(int*)$magic"
  echo "commands"
  echo "silent"
  echo "printf \"HIT magic=%d\\n\", *(int*)$magic"
  echo "bt 8"
  echo "continue"
  echo "end"
  echo "continue"
} > /tmp/gdb-job.cmd
(gdb -q -x /tmp/gdb-job.cmd build/kernel.elf > $GLOG 2>&1 &)
sleep 2

python3 tools/qemu-sendkey.py $QMP e x e c spc u p r o g ret >/dev/null 2>&1
sleep 25

echo "=== gdb ==="
head -70 $GLOG
echo "=== serial tail ==="
tail -6 $SLOG
killall -q -9 qemu-system-i386 2>/dev/null
pkill -f "gdb -q" 2>/dev/null
