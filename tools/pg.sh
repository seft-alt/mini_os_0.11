#!/bin/bash
# exec 多页：ELF 的 p_memsz 越过 4KB 时，exec 要给一段**连续**页而不是一页。
#
# 判据四条：
#   1. 格式：宿主机上确认 uprog2 的 p_memsz > 0x1000（这就是"跨页"的来源）；
#   2. 要页：`exec uprog2` rc=0、magic 自证、val=1834 —— 1834 是"第二页
#      先读回 0（说明被清过）、再写 0x2A 与 0x07、再读回"拼出来的数，
#      一页程序算不出这个数；
#   3. 页数：屏幕上出现第二行 `exec pg=2`（单页时这一行不出现）；
#   4. 归还：`heap` 的 `pf=`（物理页用量）在 exec 前后必须一样；
#      再跑一趟单页的裸二进制 `uprog`，`pg` 那行不该出现（老路径没被改坏）。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/pg-console.bin
SER=/tmp/pg-serial.log
IMG=/tmp/pg.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 0.9; }

[ -f build/uprog2.elf ] || { echo "  FAIL  build/uprog2.elf 不存在（make 应当先编出来）"; exit 1; }
python3 tools/datadisk.py $IMG build/uprog.bin '' build/uprog.elf '' build/uprog2.elf huge \
  >/tmp/datadisk-pg.log 2>&1 || { echo "  FAIL  造盘失败"; exit 1; }
sed 's/^/  /' /tmp/datadisk-pg.log

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
    return u32(img, off + 4), [u32(img, off + 8 + 4 * j) for j in range(5)]

rsize, rblocks = inode(root)
names = {}
for i in range(rsize // 16):
    eo = rblocks[0] * BLOCK + i * 16
    names[img[eo:eo + 12].split(b"\0")[0].decode("latin1")] = u32(img, eo + 12)

size, blocks = inode(names["uprog2"])
data = b""
for j in range(4):
    if blocks[j]:
        data += img[blocks[j] * BLOCK:(blocks[j] + 1) * BLOCK]
if blocks[4]:
    for k in range(128):
        b = u32(img, blocks[4] * BLOCK + k * 4)
        if b:
            data += img[b * BLOCK:(b + 1) * BLOCK]
data = data[:size]
assert data[:4] == b"\x7fELF", "uprog2 不是 ELF"
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
# 顺便报一下 exec 会算成几页（max(filesz, memsz) / 4096）
need = max(size, seg[3], seg[2] + seg[3])
print("pages=%d" % ((need + 4095) // 4096), file=sys.stderr)
PY
)
EOF
echo "  A. uprog2: e_entry=0x$(printf %x "$ENTRY") offset=$FOFF vaddr=$VADDR filesz=$FILESZ memsz=$MEMSZ"
need=$FILESZ
[ "$MEMSZ" -gt "$need" ] && need=$MEMSZ
pages=$(( (need + 4095) / 4096 ))
echo "  A2. 按 max(filesz,memsz) 算出来是 $pages 页"

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-pg.log 2>&1 &)
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

# 基线：物理页用量
type_cmd c l e a r; type_cmd h e a p; dump
pf0=$(screen | grep -oE 'pf=[0-9]+/[0-9]+' | head -1)
echo "  B. heap 基线 $pf0"

# 1. 跨页程序
type_cmd c l e a r; type_cmd e x e c spc u p r o g 2; dump
pg_out=$(screen)
echo "  C. exec uprog2："
printf '%s\n' "$pg_out" | grep -E 'exec |uprog2' | sed 's/^/    /'

# 2. 单页裸二进制（老路径）
type_cmd c l e a r; type_cmd e x e c spc u p r o g; dump
raw_out=$(screen)
raw_line=$(printf '%s\n' "$raw_out" | grep -oE 'exec uprog rc=[0-9E]+ .*' | head -1)
echo "  D. $raw_line"

# 3. 超过 16KB 的文件：应当被 -2 挡住，且不许留下页
type_cmd c l e a r; type_cmd e x e c spc h u g e; dump
huge_line=$(screen | grep -oE 'exec huge rc=[0-9E]+ .*' | head -1)
echo "  E. $huge_line"

# 4. 页都回来了吗
type_cmd c l e a r; type_cmd h e a p; dump
pf1=$(screen | grep -oE 'pf=[0-9]+/[0-9]+' | head -1)
echo "  F. heap 收尾 $pf1"

killall -q -9 qemu-system-i386 2>/dev/null

fail=0
pg_line=$(printf '%s\n' "$pg_out" | grep -oE 'exec uprog2 rc=[0-9E]+ .*' | head -1)
printf '%s\n' "$pg_line" | grep -q 'rc=0' \
  || { echo "  FAIL  exec uprog2 没成功：'$pg_line'"; fail=1; }
printf '%s\n' "$pg_line" | grep -q 'mag=0x45584543' \
  || { echo "  FAIL  跨页程序没写指纹"; fail=1; }
printf '%s\n' "$pg_line" | grep -q 'val=1834' \
  || { echo "  FAIL  val 不是 1834（第二页没读回 0x2A/0x07）：'$pg_line'"; fail=1; }
printf '%s\n' "$pg_line" | grep -q 'ex=42' \
  || { echo "  FAIL  ex 不是 42"; fail=1; }
printf '%s\n' "$pg_out" | grep -q 'uprog2:pg2 ok' \
  || { echo "  FAIL  跨页程序没有写出 uprog2:pg2 ok"; fail=1; }
printf '%s\n' "$pg_out" | grep -q 'exec pg=2' \
  || { echo "  FAIL  屏幕上没有 'exec pg=2'（exec 没按两页要）"; fail=1; }
printf '%s\n' "$raw_line" | grep -q 'rc=0' \
  || { echo "  FAIL  单页裸二进制被带坏了：'$raw_line'"; fail=1; }
printf '%s\n' "$raw_out" | grep -q 'exec pg=' \
  && { echo "  FAIL  单页程序也打了 pg 行（页数算错了）"; fail=1; }
printf '%s\n' "$huge_line" | grep -q 'rc=E2' \
  || { echo "  FAIL  20KB 的文件没被 -2 挡住：'$huge_line'"; fail=1; }
[ "$pf0" = "$pf1" ] \
  || { echo "  FAIL  物理页没还干净：$pf0 -> $pf1"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  exec 多页：跨页 ELF 要到连续两页（pg=2 / val=1834）、页全归还、单页与超限路径不变"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
