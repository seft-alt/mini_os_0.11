#!/bin/bash
# 验证“每个 owner 一条环”：一个人打满只丢自己的，别人照样进得去。
#
# 证据不在屏幕上（24 行已排满），在内存里：main 的键盘自检 ④ 做了两步——
#   1) attach 槽 1 → 踢它连喂 6 个字符（环只有 4 格）→ 它装满、丢 2 个；
#   2) 切回共享 → 再喂 4 个字符 → 共享那条环收下 4 个，一格没被槽 1 挤掉。
# 四个结果留在全局变量里，这里只负责读出来对表：
#   lane_peak1 = 4   —— 槽 1 那条环真的被塞到过 4 格（满了）
#   lane_drop1 = 2   —— 6 进 4，多出来的 2 个被丢（丢的是自己的）
#   lane_q1    = 4   —— 此时槽 1 环里还有 4 个
#   lane_q0    = 4   —— 同一时刻共享环里也有 4 个（**没被挤掉**）
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-lane.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p')
  eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 64))" ]; then
    echo "  已到 kernel_idle（EIP=0x$eip）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

peak=$(rd lane_peak1); peak=$((peak))
drop=$(rd lane_drop1); drop=$((drop))
q1=$(rd lane_q1);      q1=$((q1))
q0=$(rd lane_q0);      q0=$((q0))

echo "  lane_peak1=$peak lane_drop1=$drop lane_q1=$q1 lane_q0=$q0"

fail=0
[ "$peak" = "4" ] || { echo "  FAIL  槽 1 那条环应被塞到 4，实际 $peak"; fail=1; }
[ "$drop" = "2" ] || { echo "  FAIL  槽 1 应因满丢 2 个（6 进 4），实际 $drop"; fail=1; }
[ "$q1" = "4" ]   || { echo "  FAIL  槽 1 环里应还有 4 个，实际 $q1"; fail=1; }
[ "$q0" = "4" ]   || { echo "  FAIL  共享环应也收下 4 个（没被槽 1 挤掉），实际 $q0"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  每 owner 一条环：自己装满只丢自己，别人的 4 格不受影响"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
