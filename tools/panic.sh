#!/bin/bash
# panic：统一停机出口真的能用吗。
#
# 平时永远跑不到 panic，所以它有没有生效只能靠"故意按一下"：
# 内核里有一条 `panic <原因>` 命令（shell 命令表最后一项），按下它就会走
# 同一条出口 —— 打印 PANIC 行、cli、hlt。
#
# 判据（三个通道都要有）：
#   1. 屏幕上出现 `PANIC: probe eip=0x........`；
#   2. 串口文件里出现同一行（机器停在 QEMU 里时，串口是唯一还拿得到的东西）；
#   3. 机器真的停住了：连读两次 EIP 完全一样（hlt 死循环，不再走动）。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/panic-console.bin
SER=/tmp/panic-serial.log
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
# panic 把 CRTC 切回了第 0 页（证据页）——panic 行就落在这一页上，所以这里读 0xb8000，
# 而不是平时 shell 用的 0xb9000。
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb8000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-panic.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(eip_now); eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 120))" ]; then
    echo "  已到 kernel_idle（EIP=0x$(printf %08x $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

type_cmd p a n i c spc p r o b e; sleep 1.5
dump
scr=$(screen | grep -oE 'PANIC: probe eip=0x[0-9A-F]{8}' | head -1)
echo "  证据页（panic 后 CRTC 指向的那一页）：${scr:-（没有）}"
ser=$(grep -oE 'PANIC: probe eip=0x[0-9A-F]{8}' "$SER" | head -1)
echo "  串口：${ser:-（没有）}"
e1=$(eip_now); sleep 1; e2=$(eip_now)
echo "  停机：EIP 两次读 = 0x${e1:-?} / 0x${e2:-?}"

fail=0
[ -n "$scr" ] || { echo "  FAIL  屏幕上没有 PANIC 行"; fail=1; }
[ -n "$ser" ] || { echo "  FAIL  串口里没有 PANIC 行（机器停了就只剩这一份记录）"; fail=1; }
[ -n "${e1:-}" ] && [ "$e1" = "$e2" ] \
  || { echo "  FAIL  EIP 还在变（没有真的停住）"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  panic：屏幕与串口都留下 PANIC 行，EIP 停在同一处不再走动"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
