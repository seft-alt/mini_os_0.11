#!/bin/bash
# 设备节点：`/dev/tty` 与 `/dev/hd` 走的是同一层 struct file_operations。
#
# 这一层为什么值得单独回归：`fd_open` 之前只认"盘上的普通文件"，
# 所有读写的实现都写死在 fd.c 里。拆出 fops 之后，必须证明
# "名字分派"与"三种实现各走各的"两件事都真在跑：
#   A. `dev tty w=4`     —— 往 /dev/tty 写 4 字节，tty 层的计数器 +4；
#                           写的是 COM1，所以第 1 页的屏幕证据不受影响；
#   B. `dev hd ...`      —— /dev/hd 打开时按扇区数算出容量（1MB），
#                           读 LBA0 读到 512 字节，尾签名是盘上的真字节；
#   C. `dev bad=E3 on=0` —— 名字不在设备表、盘上也没有 → FD_E_NOENT；
#                           三个 fd 都关干净，现开着的回到 0；
#   D. `fd /dev/hd`      —— 从 fd 命令打开同一个设备：kind=hd、ino=2
#                           （设备号，不是 inode 号）。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/dev-console.bin
SER=/tmp/dev-serial.log
IMG=/tmp/dev.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

# 造一张真盘：1MB raw，第 0 扇区写已知图案 + MBR 尾签名 0x55AA。
# 故意不用 datadisk.py：mosfs 的盘把 LBA0 空着（卷从 LBA1 起），
# 而 `/dev/hd` 要证明的正是"绕过 mosfs、按扇区读原始字节"，
# 所以这里需要 LBA0 上有可断言的内容。
python3 - "$IMG" <<'PY'
import sys
p = sys.argv[1]
buf = bytearray(1024 * 1024)
buf[0:16] = b"MINI-OS DEV TEST"
buf[510] = 0x55
buf[511] = 0xAA
open(p, "wb").write(buf)
PY
[ -s $IMG ] || { echo "  FAIL  造盘失败"; exit 1; }
echo "  盘：$IMG（1MB，LBA0 图案 + 0x55AA）"

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-exec-dev.log 2>&1 &)
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

type_cmd c l e a r; type_cmd d e v; dump
t=$(screen | grep -oE 'dev tty w=[0-9]+' | head -1)
h=$(screen | grep -oE 'dev hd sz=[0-9]+ n=[0-9]+ sig=0x[0-9A-F]{4}' | head -1)
b=$(screen | grep -oE 'dev bad=E[0-9]+ on=[0-9]+' | head -1)
echo "  A. ${t:-（没有 tty 行）}"
echo "  B. ${h:-（没有 hd 行）}"
echo "  C. ${b:-（没有 bad 行）}"

type_cmd c l e a r; type_cmd f d spc slash d e v slash h d; dump
k=$(screen | grep -oE 'fd open rc=0 fd=[0-9]+ ino=[0-9]+ sz=[0-9]+ k=[a-z]+' | head -1)
echo "  D. ${k:-（没有 fd 打开行）}"

fail=0
[ "$t" = "dev tty w=4" ] || { echo "  FAIL  tty 没写进 4 个字节：'$t'"; fail=1; }
[ "$h" = "dev hd sz=1048576 n=512 sig=0xAA55" ] \
  || { echo "  FAIL  hd 容量/读取/签名有一项不对：'$h'"; fail=1; }
[ "$b" = "dev bad=E3 on=0" ] \
  || { echo "  FAIL  不存在的设备名没按 FD_E_NOENT 拒，或格子没收干净：'$b'"; fail=1; }
printf '%s\n' "$k" | grep -q 'ino=2 sz=1048576 k=hd' \
  || { echo "  FAIL  fd 命令打开设备拿到的不是 (设备号2, 1MB, hd)：'$k'"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  设备节点：/dev/tty 写串口计数 +4、/dev/hd 读出 1MB 容量与 0xAA55、"
  echo "      未知名按 FD_E_NOENT 拒、fd 表回到 0、同一设备走同一条 fops"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
