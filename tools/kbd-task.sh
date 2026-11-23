#!/bin/bash
# 验证“键盘归属随调度器走”：
#   任务创建时领一个 owner 槽（task_t.kbd_tag），调度器换人时调 kbd_owner_force 把
#   “当前归属”搬到新任务的标签上，任务回收时还槽。
#
# 证据不在屏幕上，而是三个计数（keyboard.c 里的静态变量，nm 能拿到地址）：
#   owner_switches —— 归属真的变了几次      期望 > 0
#   owner_seen     —— bit n = 槽 n 当过主人  期望 bit1..bit3 都出现过（>= 0x0E）
#   owner_last     —— 最后生效的归属        期望 0（收工回 main/共享）
# 再加一条抽查：task_kbd_tag_of(0) 必须是 0（main 不占槽）。
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
   -qmp unix:$QMP,server,nowait >/tmp/q-task.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p')
  eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 64))" ]; then
    echo "  已到 kernel_idle（EIP=$(printf 0x%08X $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

sw=$(rd owner_switches); sw=$((sw))
seen=$(rd owner_seen);   seen=$((seen))
last=$(rd owner_last);   last=$((last))

echo "  owner_switches=$sw owner_seen=$(printf 0x%08X $seen) owner_last=$last"

fail=0
[ "$sw" -gt 0 ]       || { echo "  FAIL  归属从没切换过（owner_switches=0）"; fail=1; }
[ $((seen & 0x0E)) -eq 14 ] || { echo "  FAIL  槽 1/2/3 都应真的当过主人（seen=$(printf 0x%X $seen)）"; fail=1; }
[ "$last" = "0" ]     || { echo "  FAIL  收工后归属应回到 0，实际 $last"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  键盘归属随调度器走：换人即换归属、三个任务槽都当过主人、收工回共享"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
