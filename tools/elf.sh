#!/bin/bash
# ELF 装载：盘上的文件头是 ELF 时，exec 按 PT_LOAD 搬段、用 e_entry 起跑。
#
# 判据三条，一条比一条硬：
#   1. 格式：宿主机上直接确认 uprogel 真的是 ET_EXEC/EM_386 的 ELF32，并取出 e_entry；
#   2. 分派：`exec uprogel` 的 ep= 等于那个 e_entry（不是 0），rc=0、magic 自证、val=36；
#   3. 老路径没坏：同一个 .asm 编出来的裸二进制 `uprog` 仍然 ep=0x0、rc=0。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/elf-console.bin
SER=/tmp/elf-serial.log
IMG=/tmp/elf.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 0.9; }

[ -f build/uprog.elf ] || { echo "  FAIL  build/uprog.elf 不存在（make 应当先编出来）"; exit 1; }
python3 tools/datadisk.py $IMG build/uprog.bin build/badprog.bin build/uprog.elf \
  >/tmp/datadisk-elf.log 2>&1 || { echo "  FAIL  造盘失败"; exit 1; }
sed 's/^/  /' /tmp/datadisk-elf.log

# 0. 先看的是什么：ELF 头里的 e_entry、段表里那个 PT_LOAD 的范围
read -r ENTRY FOFF VADDR FILESZ MEMSZ <<EOF
$(python3 - "$IMG" <<'PY'
import sys
BLOCK = 512
img = open(sys.argv[1], "rb").read()

def u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")

sb = BLOCK
inode_start = u32(img, sb + 20)
per_blk = u32(img, sb + 24)
root = u32(img, sb + 16)

def inode(ino):
    off = inode_start * BLOCK + (ino // per_blk) * BLOCK + (ino % per_blk) * 32
    return u32(img, off + 4), [u32(img, off + 8 + 4 * j) for j in range(4)]

rsize, rblocks = inode(root)
names = {}
for i in range(rsize // 16):
    eo = rblocks[0] * BLOCK + i * 16
    names[img[eo:eo + 12].split(b"\0")[0].decode("latin1")] = u32(img, eo + 12)

size, blocks = inode(names["uprogel"])
data = b"".join(img[b * BLOCK:(b + 1) * BLOCK] for b in blocks if b)[:size]
assert data[:4] == b"\x7fELF", "uprogel 不是 ELF"
assert int.from_bytes(data[16:18], "little") == 2, "不是 ET_EXEC"
assert int.from_bytes(data[18:20], "little") == 3, "不是 EM_386"
phoff = u32(data, 28)
phnum = int.from_bytes(data[44:46], "little")
seg = None
for i in range(phnum):
    ph = phoff + i * 32
    if u32(data, ph) == 1:
        seg = (u32(data, ph + 4), u32(data, ph + 8),
               u32(data, ph + 16), u32(data, ph + 20))
        break
print("%d %d %d %d %d" % ((u32(data, 24),) + seg))
PY
)
EOF
echo "  A. ELF: e_entry=0x$(printf %x "$ENTRY") PT_LOAD offset=$FOFF vaddr=$VADDR filesz=$FILESZ memsz=$MEMSZ"

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-elf.log 2>&1 &)
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

# 1. ELF：应当 ep=e_entry
type_cmd c l e a r; type_cmd e x e c spc u p r o g e l; dump
elf_out=$(screen)
elf_line=$(printf '%s\n' "$elf_out" | grep -oE 'exec uprogel rc=[0-9E]+ .*' | head -1)
echo "  B. $elf_line"

# 2. 裸二进制：ep 应当是 0x0
type_cmd c l e a r; type_cmd e x e c spc u p r o g; dump
raw_out=$(screen)
raw_line=$(printf '%s\n' "$raw_out" | grep -oE 'exec uprog rc=[0-9E]+ .*' | head -1)
echo "  C. $raw_line"

fail=0
printf '%s\n' "$elf_out" | grep -q 'uprog:ring3 ok' \
  || { echo "  FAIL  ELF 程序没有写出 uprog:ring3 ok"; fail=1; }
printf '%s\n' "$elf_line" | grep -q 'rc=0' \
  || { echo "  FAIL  exec uprogel 没成功：'$elf_line'"; fail=1; }
printf '%s\n' "$elf_line" | grep -q 'mag=0x45584543' \
  || { echo "  FAIL  ELF 程序没写指纹"; fail=1; }
printf '%s\n' "$elf_line" | grep -q 'val=36' \
  || { echo "  FAIL  ELF 程序的 val 不是 36"; fail=1; }
# ep 挪到了 exec 的第二行（`exec u=<n> lh=<n> ep=0x<4位>`）：主行加上它就有 81 列，
# 79 列上限会截掉尾巴。取法**不按整行正则**（上轮加了个 lh= 就把这里撞坏了），
# 而是「按空格切字段、再按 k=v 取 ep」——以后再加字段也不会坏。
exec_field() {   # $1 = 屏幕文本，$2 = 字段名（不含 '='）
  printf '%s\n' "$1" | grep -E '^row +[0-9]+\| exec ' \
    | tr ' ' '\n' | grep -m1 "^$2=" | sed "s/^$2=//"
}
ep_field=$(exec_field "$elf_out" ep)
ep=${ep_field#0x}
[ -n "$ep" ] || { echo "  FAIL  ELF 那一趟没有 ep 字段"; fail=1; }
[ "$((0x${ep:-0}))" = "$ENTRY" ] \
  || { echo "  FAIL  ep=0x$ep 与 ELF 的 e_entry=0x$(printf %x "$ENTRY") 不等"; fail=1; }
epr_field=$(exec_field "$raw_out" ep)
epr=${epr_field#0x}
[ "$((0x${epr:-1}))" = 0 ] \
  || { echo "  FAIL  裸二进制的 ep 应当是 0，实际 0x${epr:-?}"; fail=1; }
printf '%s\n' "$raw_line" | grep -q 'rc=0' \
  || { echo "  FAIL  裸二进制那条被 ELF 改动带坏了：'$raw_line'"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  ELF 装载：e_entry 起跑（ep=0x$ep）/ 指纹与结果自证 / 裸二进制老路径不变"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]

