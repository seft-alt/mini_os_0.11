#!/bin/bash
# ATA PIO：内核能不能把磁盘上的字节原样拿到内存里。
#
# 造一张真盘（不是软盘）：1MB raw 镜像，第 0 扇区写上已知图案 + MBR 尾签名 0x55AA，
# 再挂成 IDE 主通道的从盘（-drive if=ide,index=1）。
# 判据分三层，缺一层就不算通：
#   1. 认得盘：mask 有从盘位、IDENTIFY 报的扇区数 = 0x800（1MB / 512）；
#   2. 读得回：rc=0，且屏幕上的前 8 字节和图案对得上；
#   3. 内存里也对：从符号 sec_buf 读回 16 字节，与镜像文件前 16 字节逐位相等
#      —— 屏幕只能证明"渲染对"，"内核真读到了"得看内存。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/disk-console.bin
SER=/tmp/disk-serial.log
IMG=/tmp/ata.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  [ -n "$a" ] || { echo 0; return; }
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
s2s() {   # xp 的字节序列 -> 大写十六进制字符串（如 "4D 49 4E"）
  tr -d '\r' | sed -n 's/.*: //p' | python3 -c 'import sys
print(" ".join("%02X" % int(t,16) for t in sys.stdin.read().split()))'
}
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }

# 造盘：全零 1MB，第 0 扇区写入图案（前 16 字节）+ 尾签名
python3 - "$IMG" <<'PY'
import sys
p = sys.argv[1]
buf = bytearray(1024 * 1024)
buf[0:16] = b"MINI-OS ATA TEST"
buf[510] = 0x55
buf[511] = 0xAA
# 第 1 扇区放一个哨兵串，用来证明"读的是 LBA，不是永远第 0 个"
buf[512:528] = b"SECTOR-ONE-MARK!"
open(p, "wb").write(buf)
PY

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-disk.log 2>&1 &)
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

echo "  A. 串口里的启动痕迹：$(grep -c '\[ata\]' $SER) 行 [ata]，内容：$(grep '\[ata\]' $SER | head -1)"

python3 tools/qemu-sendkey.py $QMP d i s k ret >/tmp/keys-disk.log 2>&1
sleep 1.0
dump
screen | grep -E 'ata m=|drv=|d0:|sig=' | head -4

a1=$(screen | grep -oE 'ata m=0x[0-9] s=0x[0-9] sec=0x[0-9A-F]{6}' | head -1)
a2=$(screen | grep -oE 'drv=[01] lba=0x[0-9A-F]{6} rc=0 rd=[0-9]+ er=[0-9]+' | head -1)
a3=$(screen | grep -oE 'd0:([ ]([0-9A-F]{2}))+' | tail -1)
a4=$(screen | grep -oE 'sig=0x[0-9A-F]{4}' | tail -1)

# 内存侧：sec_buf 前 16 字节 vs 镜像文件前 16 字节
sb=$(sym sec_buf)
got=$(python3 tools/qemu-qmp.py $QMP "xp /16bx 0x$sb" | s2s)
want16=$(od -An -tx1 -N16 $IMG | tr 'a-f' 'A-F' | tr -s ' ' | sed 's/^ //; s/ $//')
echo "  C. 内存 sec_buf 前 16 字节：[$got]"
echo "     镜像文件前 16 字节    ：[$want16]"

# 第二个扇区：证明 LBA 参数真的传到了盘
python3 tools/qemu-sendkey.py $QMP d i s k spc 1 ret >/tmp/keys-disk2.log 2>&1
sleep 1.0
dump
b3=$(screen | grep -oE 'd0:([ ]([0-9A-F]{2}))+' | tail -1)
got2=$(python3 tools/qemu-qmp.py $QMP "xp /16bx 0x$sb" | s2s)
want2=$(od -An -tx1 -j512 -N16 $IMG | tr 'a-f' 'A-F' | tr -s ' ' | sed 's/^ //; s/ $//')
echo "  D. LBA=1 读回：[$got2]（应为 [$want2]）"

fail=0
[ -n "$a1" ] || { echo "  FAIL  disk 第一行没出来"; fail=1; }
printf '%s\n' "$a1" | grep -qE 's=0x1 sec=0x000800' \
  || { echo "  FAIL  应认出从盘且扇区数 0x800，实际 '$a1'"; fail=1; }
[ -n "$a2" ] || { echo "  FAIL  第二行 rc 不是 0（实际 '$(screen | grep -oE 'drv=.*' | head -1)'）"; fail=1; }
[ "$a4" = "sig=0xAA55" ] || { echo "  FAIL  扇区尾签名不是 0xAA55（实际 '$a4'）"; fail=1; }
printf '%s\n' "$a3" | grep -qE '4D 49 4E 49 2D 4F 53 20' \
  || { echo "  FAIL  屏上前 8 字节不是 MINI-OS（实际 '$a3'）"; fail=1; }
[ "$got" = "$want16" ] || { echo "  FAIL  内存 16 字节与镜像不一致"; fail=1; }
[ "$got2" = "$want2" ] || { echo "  FAIL  LBA=1 读回的不是第 2 扇区"; fail=1; }
grep -q '\[ata\] m=0 s=1 sec=0x000800' $SER \
  || { echo "  FAIL  串口里没有 [ata] m=0 s=1 sec=0x000800"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  ATA PIO：认盘(0x800 扇区) + 读 LBA0/LBA1 + 内存与文件逐位一致"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
