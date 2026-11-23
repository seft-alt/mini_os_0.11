#!/bin/bash
# 验证“提交队列”：回车提交不再是一格，而是一圈 4 格 FIFO，先来的先被取走、旧行不被覆盖。
#
# 证据在内存里（main 的键盘自检 ⑤ 留下的四个非 static 全局）：
#   subq_peak  = 4          —— 连压 4 条后 pending 到 4（容量真的用得满）
#   subq_order = 0x64636261 —— 依次取出的 4 条首字符 a,b,c,d（顺序没乱、旧行没被盖）
#   subq_full  = 0          —— 第 5 条 push 的返回值：队满被丢
#   subq_drop  = 1          —— 只丢了新的那一条
# 另外读 submit_pending：自检已经把队列清干净，开机后这里应为 0。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
hx() { printf '%08X' "$1"; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-subq.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p')
  eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 64))" ]; then
    echo "  已到 kernel_idle（EIP=0x$(hx $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

peak=$(rd subq_peak);      peak=$((peak))
order=$(rd subq_order);    order=$((order))
full=$(rd subq_full);      full=$((full))
drop=$(rd subq_drop);      drop=$((drop))
pend=$(rd submit_pending); pend=$((pend))

echo "  subq_peak=$peak subq_order=0x$(hx $order) subq_full=$full subq_drop=$drop submit_pending=$pend"

fail=0
[ "$peak" = "4" ]            || { echo "  FAIL  连压 4 条后 pending 应为 4，实际 $peak"; fail=1; }
[ "$order" = "1684234849" ]  || { echo "  FAIL  取出顺序应为 a,b,c,d（0x64636261），实际 0x$(hx $order)"; fail=1; }
[ "$full" = "0" ]            || { echo "  FAIL  第 5 条应被队满拒绝（返回 0），实际 $full"; fail=1; }
[ "$drop" = "1" ]            || { echo "  FAIL  应只丢新的那一条（drop=1），实际 $drop"; fail=1; }
[ "$pend" = "0" ]            || { echo "  FAIL  自检应把队列清干净（pending=0），实际 $pend"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  提交队列：4 格 FIFO 排得满、顺序不乱、满了只丢新的那条"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
