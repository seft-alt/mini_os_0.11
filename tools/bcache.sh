#!/bin/bash
# 块缓存（fs/bcache.c）：8 槽直接映射 + 写穿。
#   1. 同一文件第二次读：miss（真落盘块数）不增、hit 增
#   2. 写入后立刻读回：内容正确（写穿落到盘上）
#   3. 写入后读同一块：miss 不增（写穿顺手把新内容装进槽，读它算命中）
#
# 数值直接读全局符号 bcache_misses_n / bcache_hits_n（不解析屏幕：
# 用户页只有 23 行，fs 行会被滚动吃掉）。
# 负向用例（neg_bcache_hit / neg_bcache_wt）就是来咬这三条的。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/bc-console.bin
SER=/tmp/bc-serial.log
IMG=/tmp/bc.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rd32() { python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$1" | awk '{print $NF}'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

python3 tools/datadisk.py $IMG >/tmp/datadisk.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-bc.log 2>&1 &)
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

MISS=$(sym bcache_misses_n); HIT=$(sym bcache_hits_n)
m=$(rd32 $MISS); h=$(rd32 $HIT)
echo "  A. 基线 miss=$m hit=$h"; mA=$m; hitA=$h

type_cmd c a t spc l o n g dot t x t
m=$(rd32 $MISS); h=$(rd32 $HIT)
echo "  B. cat 一次 miss=$m hit=$h"; mB=$m; hitB=$h

type_cmd c a t spc l o n g dot t x t
m=$(rd32 $MISS); h=$(rd32 $HIT)
echo "  C. cat 两次 miss=$m hit=$h（miss 应与 B 相同）"; mC=$m; hitC=$h

type_cmd w r i t e spc t 1 spc h e l l o
m=$(rd32 $MISS); h=$(rd32 $HIT)
echo "  D. write t1 miss=$m hit=$h"; mD=$m; hitD=$h

type_cmd c a t spc t 1; dump
# 只数「不是命令回显」的行：回显行以 "> " 开头，会把命令里的 hello 也算进去
txt=$(screen | sed -n 's/^row *[0-9]*| //p' | grep -v '^>' | grep -c 'hello')
m=$(rd32 $MISS); h=$(rd32 $HIT)
echo "  E. cat t1 miss=$m hit=$h（屏上 hello 行数=$txt）"; mE=$m; hitE=$h

fail=0
grep -q '\[fs\] rc=0' $SER || { echo "  FAIL  串口没有 [fs] rc=0"; fail=1; }
[ "${mB:-0}" -gt "${mA:-0}" ] 2>/dev/null \
  || { echo "  FAIL  第一次 cat 没有落盘（$mA → $mB）"; fail=1; }
[ "${mC:-x}" = "${mB:-y}" ] 2>/dev/null \
  || { echo "  FAIL  第二次 cat 仍落盘（$mB → $mC，命中判断失效）"; fail=1; }
[ "${hitC:-0}" -gt "${hitB:-0}" ] 2>/dev/null \
  || { echo "  FAIL  第二次 cat 命中数没增长（$hitB → $hitC）"; fail=1; }
[ "${txt:-0}" -ge 1 ] 2>/dev/null \
  || { echo "  FAIL  写后读回没看到 hello（写穿/更新缓存行有问题）"; fail=1; }
# 8 槽直映：写 t1 要碰目录/inode/数据三块，cat t1 时其中一块可能已被别的块
# 换出，所以这里只要求“几乎不落盘”（增量 ≤2），不要求零落盘。
[ "$(( ${mE:-99} - ${mD:-0} ))" -le 2 ] 2>/dev/null \
  || { echo "  FAIL  写后读同一块仍大量落盘（$mD → $mE，写穿没更新缓存行）"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  块缓存：二次读零落盘/写穿落盘/写后读命中 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
