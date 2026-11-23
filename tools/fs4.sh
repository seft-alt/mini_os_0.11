#!/bin/bash
# mosfs 间接块：单文件从 2KB（4 个直接块）抬到 66KB（+1 个间接块 = 128 个块号）。
#
# 判据：
#   1. 读：`cat big3k`（3000 字节 = 6 块）能看到第 60 行 L059——第 2048 字节之后的内容，
#      只能从间接块里取块号，所以这一行在屏上就等于"间接块读对了"；
#   2. 写：`write full2k z`（文件正好 2048 字节 = 4 个直接块满）→ size 变 2050，
#      这一步必然要新建一个间接块、再新建第 5 个数据块；
#   3. 落盘（关掉 QEMU 后读 /tmp/fs4.img）：
#      big3k 的 3000 字节逐字节等于造盘时的内容；full2k 的 inode 里 blocks[4] 是间接块、
#      间接块第 0 项指向放 "z\n" 的数据块、size=2050；相关的位图 bit 都置了。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/fs4-console.bin
SER=/tmp/fs4-serial.log
IMG=/tmp/fs4.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 0.9; }

python3 tools/datadisk.py $IMG build/uprog.bin '' '' big >/tmp/datadisk4.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }
sed 's/^/  /' /tmp/datadisk4.log

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-fs4.log 2>&1 &)
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

# 1. 读 3000 字节的文件：屏上只看得到末尾几行，所以在不在 L059 是关键
type_cmd c l e a r; type_cmd c a t spc b i g 3 k; dump
cat_out=$(screen)
echo "  A. cat big3k 尾部："
printf '%s\n' "$cat_out" | grep -E 'L0[0-9]{2}' | tail -3 | sed 's/^/    /'
type_cmd c l e a r; type_cmd f s; dump
fsline=$(screen | grep -oE 'fs ok=0x[0-9] b=[0-9]+ i=[0-9]+ rd=[0-9]+ er=[0-9]+ wr=[0-9]+ cr=[0-9]+ un=[0-9]+' | head -1)
echo "  B. $fsline"

# 2. 写第 2049 个字节：4 个直接块已经满了，必须走间接块
type_cmd c l e a r; type_cmd w r i t e spc f u l l 2 k spc z; dump
wline=$(screen | grep -oE 'write full2k rc=0 .*' | head -1)
echo "  C. $wline"
type_cmd c l e a r; type_cmd l s; dump
ls_out=$(screen)
echo "  D. ls:"
printf '%s\n' "$ls_out" | grep -E '\| (big3k|full2k)' | sed 's/^/    /'

# 3. 删 6 块的文件：间接块本身也必须还回位图
type_cmd c l e a r; type_cmd r m spc b i g 3 k; dump
rmline=$(screen | grep -oE 'rm big3k rc=0 .*' | head -1)
echo "  E. $rmline"
type_cmd c l e a r; type_cmd l s; dump
ls_out2=$(screen)
echo "  F. 删完的 ls："
printf '%s\n' "$ls_out2" | sed -n '1,8p' | sed 's/^/    /'

killall -q -9 qemu-system-i386 2>/dev/null; sleep 1
python3 - "$IMG" <<'PY' > /tmp/fs4-disk.log 2>&1
import sys
BLOCK = 512
img = open(sys.argv[1], "rb").read()

def u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")

sb = BLOCK
inode_start = u32(img, sb + 20)
per_blk = u32(img, sb + 24)
bitmap = u32(img, sb + 28)
root = u32(img, sb + 16)

def inode(ino):
    off = inode_start * BLOCK + (ino // per_blk) * BLOCK + (ino % per_blk) * 32
    return (int.from_bytes(img[off:off + 2], "little"),
            int.from_bytes(img[off + 4:off + 8], "little"),
            [u32(img, off + 8 + 4 * j) for j in range(5)])

rmode, rsize, rblocks = inode(root)
names = {}
for i in range(rsize // 16):
    eo = rblocks[0] * BLOCK + i * 16
    names[img[eo:eo + 12].split(b"\0")[0].decode("latin1")] = u32(img, eo + 12)

def bit_set(b):
    return bool(img[bitmap * BLOCK + b // 8] & (1 << (b % 8)))

def file_data(ino):
    mode, size, blocks = inode(ino)
    out = b""
    for i in range(4):
        if blocks[i]:
            out += img[blocks[i] * BLOCK:(blocks[i] + 1) * BLOCK]
    if blocks[4]:
        ind = blocks[4]
        for k in range(128):
            b = u32(img, ind * BLOCK + k * 4)
            if b:
                out += img[b * BLOCK:(b + 1) * BLOCK]
    return out[:size]

def file_blocks(ino):
    """这个文件占的全部块号：直接块 + 间接表里的块 + 间接块本身。"""
    mode, size, blocks = inode(ino)
    out = [b for b in blocks[:4] if b]
    if blocks[4]:
        out.append(blocks[4])
        for k in range(128):
            b = u32(img, blocks[4] * BLOCK + k * 4)
            if b:
                out.append(b)
    return out

big_blks = [9, 10, 11, 12, 15]   # 造盘时 big3k 拿到的块，第 5 个是间接块
bad = 0

print("names=%s" % sorted(names))
if "big3k" in names:
    print("  FAIL big3k 还被 rm 掉之后仍留在目录里"); bad = 1
else:
    for b in big_blks:
        if bit_set(b):
            print("  FAIL rm 之后块 %d 的位图 bit 还是 1（间接块没还回去）" % b); bad = 1
    print("big3k 的 6 块（含间接块 %d）位图 bit 全清" % big_blks[4])

mode2, size2, b2 = inode(names["full2k"])
print("full2k: mode=%d size=%d blocks=%s" % (mode2, size2, b2))
if size2 != 2050:
    print("  FAIL full2k size=%d（应为 2048 + \"z\\n\" = 2050）" % size2); bad = 1
if not b2[4]:
    print("  FAIL full2k 没有间接块（第 5 块必须从间接表来）"); bad = 1
else:
    ent0 = u32(img, b2[4] * BLOCK)
    print("full2k 间接块=%d，第 0 项 -> 数据块 %d" % (b2[4], ent0))
    if not ent0:
        print("  FAIL 间接块第 0 项还是 0"); bad = 1
    else:
        head = img[ent0 * BLOCK:ent0 * BLOCK + 8]
        print("  那个数据块开头 = %r（应为 b'z\\n'）" % head)
        if head[:2] != b"z\n":
            print("  FAIL \"z\\n\" 没落到第 5 个数据块"); bad = 1
        if not bit_set(ent0) or not bit_set(b2[4]):
            print("  FAIL 新数据块或间接块的位图 bit 没置"); bad = 1
        if file_data(names["full2k"])[:2048] != b"A" * 2048:
            print("  FAIL full2k 前 2048 字节被写坏了"); bad = 1

# 位图总账：置位的 bit 数必须正好等于"根目录 + 所有文件"占的块
used = []
missing = []
plan = [("<root>", [b for b in rblocks[:4] if b])]
plan += [(nm, file_blocks(ino)) for nm, ino in names.items()]
for nm, blks in plan:
    for b in blks:
        if b in used:
            continue
        used.append(b)
        if not bit_set(b):
            missing.append((nm, b))
if missing:
    print("  FAIL 这些块没被登记：%s" % missing); bad = 1
nbit = sum(bin(img[bitmap * BLOCK + i]).count("1") for i in range(256))
print("位图置位数=%d，按根目录+文件算出来=%d" % (nbit, len(used)))
if nbit != len(used):
    print("  FAIL 位图有 %d 个 bit 悬空（多半是 rm 漏还了块）" % (nbit - len(used))); bad = 1

print("DISK", "OK" if bad == 0 else "BAD")
sys.exit(bad)
PY
disk_rc=$?
sed 's/^/    /' /tmp/fs4-disk.log

fail=0
printf '%s\n' "$cat_out" | grep -q 'L059' \
  || { echo "  FAIL  cat big3k 没读到第 60 行（第 2048 字节之后的内容要从间接块来）"; fail=1; }
[ -n "$wline" ] || { echo "  FAIL  write full2k 没有成功行"; fail=1; }
printf '%s\n' "$wline" | grep -q 'sz=2050' \
  || { echo "  FAIL  write 之后 sz 不是 2050：'$wline'"; fail=1; }
printf '%s\n' "$ls_out" | grep -qE 'full2k +2050' \
  || { echo "  FAIL  ls 里 full2k 不是 2050"; fail=1; }
printf '%s\n' "$ls_out" | grep -qE 'big3k +3000' \
  || { echo "  FAIL  ls 里 big3k 不是 3000"; fail=1; }
[ -n "$rmline" ] || { echo "  FAIL  rm big3k 没有成功行"; fail=1; }
printf '%s\n' "$rmline" | grep -q 'n=6->5' \
  || { echo "  FAIL  rm 之后目录项数不是 6->5：'$rmline'"; fail=1; }
printf '%s\n' "$ls_out2" | grep -q 'big3k' \
  && { echo "  FAIL  rm 之后 ls 里还有 big3k"; fail=1; }
printf '%s\n' "$ls_out2" | grep -qE 'full2k +2050' \
  || { echo "  FAIL  rm 之后 full2k 也跟着不见了"; fail=1; }
[ "$disk_rc" = 0 ] || { echo "  FAIL  宿主机读盘校验没过"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  mosfs 间接块：3000 字节读得全，满 4 块之后写第 5 块自动新建间接块"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
