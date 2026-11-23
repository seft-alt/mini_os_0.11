#!/bin/bash
# 管道：两个 fd 指向同一个环，读端只读、写端只写。
#
# 判据：
#   A. `pipe` 第一行 r/w 各拿一个 fd 号、写 5 字节读回 5 字节、逐字节对上
#      （ok=1）——证明"两个格子 + 一个环 + 方向标志"这条线是通的；
#   B. 第二行 w=512 —— 600 字节只收下一个环的容量，短写不是"吃掉全部"；
#      d=88 —— 剩下 88 字节记成丢弃，账面没丢钱；
#      rd=512 —— 再从环里读回 512，证明满写时环真的满了，不是把旧字节覆盖；
#      live=0 —— 两端都关掉后槽回收，没泄漏；
#   C. 再跑一遍 `pipe`：两行仍成立且 live 仍为 0（连续两次建/拆不积累）；
#   D. `fd` 收工后 on=0 —— 管道占的两个格子也随 close 还回来了。
#
# 与 0.11 的差异是**故意**的：这里的管道非阻塞（空环读到 0、满环短写），
# 0.11 靠 task_sleep/wakeup 阻塞。见 include/linux/pipe.h 的头注释。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/pipe-console.bin
SER=/tmp/pipe-serial.log
IMG=/tmp/pipe.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

# 造一张 1MB 空盘：管道不碰盘，但 mosfs 初始化要有个设备在
python3 - "$IMG" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytearray(1024 * 1024))
PY
[ -s $IMG ] || { echo "  FAIL  造盘失败"; exit 1; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-exec-pipe.log 2>&1 &)
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

type_cmd c l e a r; type_cmd p i p e; dump
a1=$(screen | grep -oE 'pipe r=[0-9]+ w=[0-9]+ n=[0-9]+ ok=[0-9]' | head -1)
b1=$(screen | grep -oE 'pipe big w=[0-9]+ d=[0-9]+ rd=[0-9]+ live=[0-9]+' | head -1)
echo "  A. ${a1:-（没有来回行）}"
echo "  B. ${b1:-（没有写满行）}"

type_cmd c l e a r; type_cmd p i p e; dump
a2=$(screen | grep -oE 'pipe r=[0-9]+ w=[0-9]+ n=[0-9]+ ok=[0-9]' | head -1)
b2=$(screen | grep -oE 'pipe big w=[0-9]+ d=[0-9]+ rd=[0-9]+ live=[0-9]+' | head -1)
echo "  C. 再跑一遍：${a2:-（没有）} / ${b2:-（没有）}"

type_cmd c l e a r; type_cmd f d; dump
d=$(screen | grep -oE 'fd s0=[0-9]+ s1=[0-9]+ .* on=[0-9]+ .* dn=[0-9]+' | head -1)
echo "  D. 收工后的 fd 表：${d:-（没有输出）}"

fail=0
printf '%s\n' "$a1" | grep -q ' n=5 ok=1' \
  || { echo "  FAIL  5 字节没原样读回来：'$a1'"; fail=1; }
[ "$b1" = "pipe big w=512 d=88 rd=512 live=0" ] \
  || { echo "  FAIL  写满那一段不是 (512/88/512/0)：'$b1'"; fail=1; }
printf '%s\n' "$a2" | grep -q ' n=5 ok=1' \
  || { echo "  FAIL  第二遍的来回行不对（建/拆有残留？）：'$a2'"; fail=1; }
[ "$b2" = "pipe big w=512 d=88 rd=512 live=0" ] \
  || { echo "  FAIL  第二遍的写满行不对（槽没回收？）：'$b2'"; fail=1; }
printf '%s\n' "$d" | grep -q ' on=0 ' \
  || { echo "  FAIL  管道两格没还回 fd 表：'$d'"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  管道：一对 fd 共享一个 512 字节环（5 字节来回、600 字节写短写 512/丢 88、"
  echo "      读完 live=0、连跑两遍不积累、两格都还回 fd 表）"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
