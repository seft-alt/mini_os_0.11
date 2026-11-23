#!/bin/bash
# MBR 分区（kernel/blk_drv/part.c）：盘首写分区表、卷挪到 LBA 64，
# 文件系统应当从分区起点挂载，而不是整盘。
#   1. 认出 1 张 MBR、登记 1 个分区、0 条被跳过
#   2. 分区类型 0x83、分区设备 sectors=1984（整盘 2048 - 起点 64）
#   3. fs 挂载成功，且 b=1984（是分区卷的大小，不是整盘 2048）
#   4. cat 分区里的 hello.txt，内容正确
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/pt-console.bin
SER=/tmp/pt-serial.log
IMG=/tmp/pt.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rd32() { python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$1" | awk '{print $NF}'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

python3 tools/datadisk.py $IMG part=64 >/tmp/pt-datadisk.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-pt.log 2>&1 &)
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

PC=$(sym part_count_n); PM=$(sym part_mbr_n); PS=$(sym part_skipped_n)
PT=$(sym part_type_tab); TB=$(sym blkdev_sectors_tab)
pc=$(rd32 "$PC"); pm=$(rd32 "$PM"); ps=$(rd32 "$PS")
ptype=$(rd32 "$(printf %x $((0x$PT + 8)))")     # part_type_tab[2]（槽 2 是第一个分区）
psec=$(rd32 "$(printf %x $((0x$TB + 8)))")
echo "  A. part: count=$pc mbr=$pm skipped=$ps type[2]=$ptype"
echo "  B. sectors: 分区设备=$psec（整盘 2048）"

type_cmd f s; dump
fsline=$(screen | grep -oE 'fs ok=0x[0-9] b=[0-9]+' | tail -1)
echo "  C. $fsline"

type_cmd c a t spc h e l l o dot t x t; dump
txt=$(screen | sed -n 's/^row *[0-9]*| //p' | grep -v '^>' | grep -c 'hello from mosfs v1')
echo "  D. cat hello.txt：输出行数=$txt"

fail=0
grep -q '\[fs\] rc=0' $SER || { echo "  FAIL  串口没有 [fs] rc=0（分区上的卷没挂上）"; fail=1; }
[ "${pc:-0}" = 1 ] 2>/dev/null || { echo "  FAIL  part_count=$pc（应登记 1 个分区）"; fail=1; }
[ "${pm:-0}" = 1 ] 2>/dev/null || { echo "  FAIL  part_mbr=$pm（应认出 1 张 MBR）"; fail=1; }
[ "${ps:-0}" = 0 ] 2>/dev/null || { echo "  FAIL  part_skipped=$ps（不该有被跳过的分区）"; fail=1; }
[ "${ptype:-0}" = 131 ] 2>/dev/null || { echo "  FAIL  type[2]=$ptype（应为 0x83=131）"; fail=1; }
[ "${psec:-0}" = 1984 ] 2>/dev/null \
  || { echo "  FAIL  分区设备 sectors=$psec（应为 2048-64=1984）"; fail=1; }
printf '%s\n' "$fsline" | grep -q 'ok=0x1 b=1984' \
  || { echo "  FAIL  fs 没按分区卷挂载（预期 ok=0x1 b=1984，实得：$fsline）"; fail=1; }
[ "${txt:-0}" -ge 1 ] 2>/dev/null \
  || { echo "  FAIL  cat 分区里的 hello.txt 没有输出"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  MBR 分区：认表/登记/按分区起点挂载 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
