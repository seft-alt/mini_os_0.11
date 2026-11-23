#!/bin/bash
# 块设备层（kernel/blk_drv/blkdev.c）：登记表、扇区表、经块设备读。
# 数值一律用 QMP 直接读全局符号，不解析屏幕（屏幕滚动会吃掉 fs 行）。
#   1. blkdev_count_n 至少 1，且扇区表非零项数 == 登记数
#   2. mosfs 用的那块盘（dev1，IDE 从盘）扇区数 > 0
#   3. 第一次 cat：bcache_misses 增
#   4. 第二次 cat：bcache_misses 不增、bcache_hits 增
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/bk-console.bin
SER=/tmp/bk-serial.log
IMG=/tmp/bk.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rd32() { python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$1" | awk '{print $NF}'; }
screen() { python3 tools/vga-screen.py "$CON"; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }

python3 tools/datadisk.py $IMG >/tmp/datadisk.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-bk.log 2>&1 &)
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

CNT=$(rd32 "$(sym blkdev_count_n)")
TB=$(sym blkdev_sectors_tab)
S0=$(rd32 "$TB")
S1=$(rd32 "$(printf %x $((0x$TB + 4)))")
S2=$(rd32 "$(printf %x $((0x$TB + 8)))")
S3=$(rd32 "$(printf %x $((0x$TB + 12)))")
nonzero=0
for v in $S0 $S1 $S2 $S3; do [ "${v:-0}" -gt 0 ] 2>/dev/null && nonzero=$((nonzero + 1)); done
echo "  A. blkdev_count=$CNT  扇区表 [$S0 $S1 $S2 $S3]"

MISS=$(sym bcache_misses_n); HIT=$(sym bcache_hits_n)
m0=$(rd32 $MISS); h0=$(rd32 $HIT)
echo "  B. 挂载后 miss=$m0 hit=$h0"

type_cmd c a t spc h e l l o dot t x t
m1=$(rd32 $MISS); h1=$(rd32 $HIT)
echo "  C. cat 一次后 miss=$m1 hit=$h1"

type_cmd c a t spc h e l l o dot t x t
m2=$(rd32 $MISS); h2=$(rd32 $HIT); dump
echo "  D. cat 两次后 miss=$m2 hit=$h2"
screen | grep -E 'hello from|^row 24' | tail -3 | sed 's/^/     | /'

fail=0
grep -q '\[fs\] rc=0' $SER || { echo "  FAIL  串口没有 [fs] rc=0（挂载没走通）"; fail=1; }
[ "${CNT:-0}" -ge 1 ] 2>/dev/null || { echo "  FAIL  blkdev_count=$CNT（一块盘都没登记）"; fail=1; }
[ "${nonzero:-0}" = "${CNT:-x}" ] 2>/dev/null \
  || { echo "  FAIL  登记了 $CNT 块，扇区表非零项却只有 $nonzero 个"; fail=1; }
[ "${S1:-0}" -gt 0 ] 2>/dev/null || { echo "  FAIL  dev1 sectors=$S1（mosfs 用的那块盘容量为 0）"; fail=1; }
[ "${m1:-0}" -gt "${m0:-0}" ] 2>/dev/null \
  || { echo "  FAIL  第一次 cat 没有新增落盘块（$m0 → $m1）"; fail=1; }
[ "${m2:-x}" = "${m1:-y}" ] 2>/dev/null \
  || { echo "  FAIL  第二次 cat 仍落盘（$m1 → $m2，缓存没命中）"; fail=1; }
[ "${h2:-0}" -gt "${h1:-0}" ] 2>/dev/null \
  || { echo "  FAIL  第二次 cat 命中数没增长（$h1 → $h2）"; fail=1; }
screen | grep -q 'hello from' || { echo "  FAIL  cat hello.txt 没打印出内容"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  块设备：登记/扇区表/二次读零落盘 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
