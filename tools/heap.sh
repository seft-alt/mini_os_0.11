#!/bin/bash
# 内核堆：kalloc/kfree 的四步（要得到 / 写得上 / 读得回 / 还得了）。
#
# 为什么要"先 heap、再 kstress"两条命令：heap 单独打一次要看出"还没要过页"，
# kstress 之后再看 heap 才能看出"页是懒要来的"——这两次之间的差值就是证据。
# 判据：
#   1. kstress 之前 heap p=0（初始化不占页，所以 24 行自检屏的数字不受影响）；
#   2. kstress 之后 bad=0x000000（写进去的图案全读回来了）；
#   3. kstress 之后 u=0x000000、l=0（全还回去了），p>=1（页确实是要来的）；
#   4. 内存侧交叉核对：pages_taken>=1、used_bytes==0、fails==0。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/heap-console.bin
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  [ -n "$a" ] || { echo 0; return; }
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-heap.log 2>&1 &)
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

pages0=$(rd pages_taken)
echo "  A. 未用堆之前：pages_taken=$pages0（应为 0，初始化不占页）"

python3 tools/qemu-sendkey.py $QMP h e a p ret >/tmp/keys-heap1.log 2>&1
sleep 0.8
dump
h0=$(screen | grep -oE 'heap p=[0-9]+ u=0x[0-9A-F]{6} l=[0-9]+' | head -1)
echo "  heap（前）: ${h0:-（没打出来）}"

python3 tools/qemu-sendkey.py $QMP k s t r e s s ret >/tmp/keys-heap2.log 2>&1
sleep 0.8
dump
ks=$(screen | grep -oE 'kstress 8x100 bad=0x[0-9A-F]{6} u=0x[0-9A-F]{6} l=[0-9]+' | head -1)
echo "  kstress: ${ks:-（没打出来）}"
python3 tools/qemu-sendkey.py $QMP h e a p ret >/tmp/keys-heap3.log 2>&1
sleep 0.8
dump
h1=$(screen | grep -oE 'heap p=[0-9]+ u=0x[0-9A-F]{6} l=[0-9]+' | tail -1)
echo "  heap（后）: ${h1:-（没打出来）}"

pages1=$(rd pages_taken); used=$(rd used_bytes); fails=$(rd fails); leaks=$(rd leaks)
echo "  B. 内存侧：pages_taken=$pages1 used_bytes=$used fails=$fails leaks=$leaks"

fail=0
[ "$pages0" = "0" ] || { echo "  FAIL  初始化就占页了（pages_taken=$pages0），会动 24 行自检的数字"; fail=1; }
[ -n "${h0:-}" ] || { echo "  FAIL  第一次 heap 没有输出"; fail=1; }
printf '%s\n' "$h0" | grep -qE 'heap p=0 u=0x0+ l=0' \
  || { echo "  FAIL  未用堆时 heap 应为 p=0/u=0/l=0，实际 '$h0'"; fail=1; }
printf '%s\n' "$ks" | grep -qE 'bad=0x0{6} u=0x0{6} l=0' \
  || { echo "  FAIL  kstress 结果不对：'${ks:-（空）}'（bad/u/l 都应为 0）"; fail=1; }
[ -n "${h1:-}" ] || { echo "  FAIL  第二次 heap 没有输出"; fail=1; }
printf '%s\n' "$h1" | grep -qE 'heap p=[1-9][0-9]* u=0x0{6} l=0' \
  || { echo "  FAIL  kstress 之后 heap 应为 p>=1/u=0/l=0，实际 '$h1'"; fail=1; }
[ "$pages1" -ge 1 ] 2>/dev/null || { echo "  FAIL  pages_taken=$pages1，页没有真的要到"; fail=1; }
[ "$used" = "0" ]               || { echo "  FAIL  used_bytes=$used，归还后应回 0"; fail=1; }
[ "$fails" = "0" ]              || { echo "  FAIL  kalloc 失败 $fails 次"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  内核堆：懒要页（$pages0 -> $pages1）、图案校验全过、全部归还后账回 0"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
