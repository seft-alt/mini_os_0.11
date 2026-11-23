#!/bin/bash
# 串口（COM1）验证：证明 printk 的字节真的从第二条通道出去了。
#
# 判据只有两条，都是"文件里有没有"：
#   1. 串口文件里能按顺序看到启动日志（第一行是横幅，后面是各段自检）；
#   2. 行数与屏幕上的 printk 行数对得上（不要求逐字，只要"没少打"）。
# 为什么这条通道值得单独一个测试：它坏了不会影响开机——屏幕照常、自检照常，
# 只有"调试时串口文件是空的"这一个症状，很容易被当成"QEMU 没配好"。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
SER=/tmp/serial.log
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -serial file:$SER -qmp unix:$QMP,server,nowait >/tmp/q-serial.log 2>&1 &)
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
killall -q -9 qemu-system-i386 2>/dev/null
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; exit 1; }

sleep 0.5
[ -s $SER ] || { echo "  FAIL  串口文件是空的（COM1 没输出）"; exit 1; }

n_lines=$(wc -l < $SER)
echo "  串口记录 $n_lines 行，前 3 行："
sed -n '1,3p' $SER | sed 's/^/    /'

fail=0
head -1 $SER | grep -q "mini-os 0.2" || { echo "  FAIL  第一行不是横幅"; fail=1; }
grep -q "\[idt\] 00 div" $SER        || { echo "  FAIL  串口里没有 idt 那行"; fail=1; }
grep -q "\[mem\] demand base=0x02000000" $SER || { echo "  FAIL  串口里没有按需分页那行"; fail=1; }
grep -q "unhandled" $SER             || { echo "  FAIL  串口里没有崩溃收尾那行"; fail=1; }
[ "$n_lines" -ge 20 ] || { echo "  FAIL  串口只收到 $n_lines 行，明显少于 24 行自检"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  串口：$n_lines 行启动日志按序落到文件，崩溃前最后一行也在"
else
  echo "FAIL  上一行是失败原因"
fi
[ "$fail" = 0 ]
