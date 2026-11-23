#!/bin/bash
# 等事件（task_wait_chan / task_wakeup）：两个 worker 用一个信箱 ping-pong 16 轮。
#
# 判据：
#   A. `kwait n=16 ok=1 to=0`
#        —— 16 轮全部完成；每轮的数据（i*7+3）逐轮对上，说明唤醒之后真的
#           跑到、读到了对端刚摆下的东西；to=0 说明没走超时兜底那条路（
#           "靠 abort 才退出"的绿是假绿，必须排除）。
#   B. `kwait w=<唤醒> k=<等待> left=0`
#        —— 等待与唤醒次数配对（每个睡下去的都被人叫回来了，没多叫也没漏叫）；
#           k >= 16 证明这趟真的睡过（不是"条件总提前成立"的空跑）；
#           left=0 是"没有槽位睡死在通道上"——这条比 ok=1 更硬：
#           漏一次唤醒，left 就会停在 1 并且这趟根本走不到 16 轮。
#
# 与 task_sleep 的区别：那个等"时间到"（睡眠堆 + wake_tick），这个等"条件成立"
# （不进任何队列，只挂 chan）。两者都要有，前者给延时，后者给同步。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/kwait-console.bin
SER=/tmp/kwait-serial.log
IMG=/tmp/kwait.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

# 一张 1MB 空盘：kwait 不碰盘，但 mosfs 初始化要有个设备在
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
   -qmp unix:$QMP,server,nowait >/tmp/q-exec-kwait.log 2>&1 &)
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

type_cmd c l e a r; type_cmd k w a i t
# 正常路径 1~2 秒就有两行；被变异打死的路径要等 main 的超时兜底（约 12 秒）。
# 所以这里轮询到"出现 kwait 行"或最多 24 秒，避免把"还在跑"误判成"没有输出"。
for i in $(seq 1 24); do
  dump
  screen | grep -q 'kwait n=' && break
  sleep 1
done
a=$(screen | grep -oE 'kwait n=[0-9]+ ok=[0-9]+ to=[0-9]+' | head -1)
b=$(screen | grep -oE 'kwait w=[0-9]+ k=[0-9]+ left=[0-9]+' | head -1)
echo "  A. ${a:-（没有轮数行）}"
echo "  B. ${b:-（没有计数行）}"

w=0
k=0
if [ -n "$b" ]; then
  w=$(printf '%s\n' "$b" | sed -n 's/.*w=\([0-9]*\).*/\1/p')
  k=$(printf '%s\n' "$b" | sed -n 's/.*k=\([0-9]*\).*/\1/p')
  echo "  B2. 唤醒=$w 等待=$k（应当相等且 ≥16）"
fi

# 槽位收工后应当全空：再打一次 tasks，看两个 worker 槽位回到 unused
type_cmd c l e a r; type_cmd t a s k s; dump
t1=$(rows | grep -E '^t1 ' | head -1)
t2=$(rows | grep -E '^t2 ' | head -1)
wleft=$(rows | grep -cE '^t[0-9] wait' || true)
echo "  C. ${t1:-（没有 t1 行）} / ${t2:-（没有 t2 行）} / 仍在等的槽位=$wleft"

fail=0
[ "$a" = "kwait n=16 ok=1 to=0" ] \
  || { echo "  FAIL  16 轮没全过、或走了超时兜底：'$a'"; fail=1; }
[ "$b" = "kwait w=$k k=$k left=0" ] \
  || { echo "  FAIL  等待/唤醒没配对、或有槽位睡死：'$b'"; fail=1; }
[ "$k" -ge 16 ] 2>/dev/null \
  || { echo "  FAIL  这趟没真睡过（k=$k）：条件总是提前成立，等于没测到等待"; fail=1; }
printf '%s\n' "$t1" | grep -q 'unused' \
  || { echo "  FAIL  writer 槽位没回到 unused：'$t1'"; fail=1; }
printf '%s\n' "$t2" | grep -q 'unused' \
  || { echo "  FAIL  reader 槽位没回到 unused：'$t2'"; fail=1; }
[ "$wleft" = 0 ] || { echo "  FAIL  收工后还有槽位是 wait 状态"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  等事件：16 轮 ping-pong 全过（数据逐轮对、无超时兜底、"
  echo "      等待/唤醒 $k/$k 配对、收工后没有槽位挂在通道上）"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
