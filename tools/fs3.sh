#!/bin/bash
# mosfs 删除：名字消失、inode 归零、块还回位图——且三件事都真的落在盘上。
#
# 判据三层：
#   1. 接口：`rm hi` → rc=0 且目录项数减 1；`rm nope` → rc=E1；`cat hi` → no such file；
#   2. 内存视图：`ls` 里没有 hi / big 了；
#   3. 落盘（关掉 QEMU 后直接解析 /tmp/fs3.img）：
#      目录项里没有这两个名字、它们的 inode mode 回到 0、
#      对应数据块在位图里的 bit 被清掉、根目录 inode 的 size 退回到造盘时的 64。
#
# 为什么第 3 层是重点：删除最容易"看着成功了"——`ls` 只读内存里的 dent[]，
# 目录块没改、位图没还，重启一次文件就"复活"了。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/fs3-console.bin
SER=/tmp/fs3-serial.log
IMG=/tmp/fs3.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 0.9; }

python3 tools/datadisk.py $IMG build/uprog.bin '' build/uprog.elf >/tmp/datadisk3.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }
sed 's/^/  /' /tmp/datadisk3.log
cp $IMG $IMG.before          # 留一份"刚造好"的盘，最后拿来比位图与根目录大小

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-fs3.log 2>&1 &)
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

echo "  A. 串口：$(grep '\[fs\]' $SER | head -1)"

# 1. 建一个小文件，然后删掉
type_cmd c l e a r; type_cmd w r i t e spc h i spc a b c d e f; dump
echo "  B. $(screen | grep -oE 'write hi rc=0 .*' | head -1)"
type_cmd c l e a r; type_cmd l s; dump
before=$(screen | grep -cF '| hi ')
type_cmd c l e a r; type_cmd r m spc h i; dump
rmline=$(screen | grep -oE 'rm hi rc=0 n=[0-9]+->[0-9]+' | head -1)
echo "  C. $rmline"
type_cmd c l e a r; type_cmd l s; dump
after_ls=$(screen)
echo "  D. ls（删后）："
printf '%s\n' "$after_ls" | grep -E '\| (hello|hi|long|blob)' | sed 's/^/    /'
type_cmd c l e a r; type_cmd c a t spc h i; dump
cat_hi=$(screen | grep -oE 'cat: no such file' | head -1)
echo "  E. cat hi: ${cat_hi:-（没有报错）}"
type_cmd c l e a r; type_cmd r m spc n o p e dot t x t; dump
rm_nope=$(screen | grep -oE 'rm nope\.txt rc=E[0-9] n=[0-9]+->[0-9]+' | head -1)
echo "  F. $rm_nope"

# 2. 建一个跨两块的文件再删：位图里两块都要被还回去
T1=01234567890123456789012
T2=abcdefghijklmnopqrstuvw
for i in $(seq 1 11); do
  keys="w r i t e spc b i g spc"
  for ((k = 0; k < ${#T1}; k++)); do keys="$keys ${T1:$k:1}"; done
  keys="$keys spc"
  for ((k = 0; k < ${#T2}; k++)); do keys="$keys ${T2:$k:1}"; done
  type_cmd $keys >/dev/null
done
type_cmd c l e a r; type_cmd l s; dump
big_line=$(screen | grep -oE 'big +[0-9]+' | head -1)
screen | grep -E 'big' | sed 's/^/    /'
echo "  G. ls: $big_line"
type_cmd c l e a r; type_cmd r m spc b i g; dump
rmbig=$(screen | grep -oE 'rm big rc=0 n=[0-9]+->[0-9]+' | head -1)
echo "  H. ${rmbig:-（rm big 没有成功行）}"
type_cmd c l e a r; type_cmd f s; dump
fsline=$(screen | grep -oE 'fs ok=0x[0-9] b=[0-9]+ i=[0-9]+ rd=[0-9]+ er=[0-9]+ wr=[0-9]+ cr=[0-9]+ un=[0-9]+' | head -1)
echo "  I. $fsline"

# 3. 落盘校验
killall -q -9 qemu-system-i386 2>/dev/null; sleep 1
python3 - "$IMG.before" "$IMG" <<'PY' > /tmp/fs3-disk.log 2>&1
import sys
BLOCK = 512
before = open(sys.argv[1], "rb").read()
after = open(sys.argv[2], "rb").read()

def u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")

sb = BLOCK
inode_start = u32(after, sb + 20)
per_blk = u32(after, sb + 24)
bitmap = u32(after, sb + 28)
root = u32(after, sb + 16)

def inode(img, ino):
    off = inode_start * BLOCK + (ino // per_blk) * BLOCK + (ino % per_blk) * 32
    return (int.from_bytes(img[off:off + 2], "little"),
            int.from_bytes(img[off + 4:off + 8], "little"),
            [u32(img, off + 8 + 4 * j) for j in range(4)])

def names_of(img):
    rmode, rsize, rblocks = inode(img, root)
    out = {}
    for i in range(rsize // 16):
        eo = rblocks[0] * BLOCK + i * 16
        out[img[eo:eo + 12].split(b"\0")[0].decode("latin1")] = u32(img, eo + 12)
    return rsize, out

bsize, bnames = names_of(before)
asize, anames = names_of(after)
print("root size: before=%d after=%d" % (bsize, asize))
print("names before=%s" % sorted(bnames))
print("names after =%s" % sorted(anames))

bad = 0
if bsize != asize:
    print("  FAIL 根目录大小变了（%d -> %d）：说明目录项的抽/补不对" % (bsize, asize)); bad = 1
if sorted(bnames) != sorted(anames):
    print("  FAIL 目录项集合变了（hi/big 之外还有东西被动过）"); bad = 1

bm_before = before[bitmap * BLOCK:(bitmap + 1) * BLOCK]
bm_after = after[bitmap * BLOCK:(bitmap + 1) * BLOCK]
if bm_before == bm_after:
    print("位图：删完之后与造盘时逐字节相同（块全部还回去了）")
else:
    diff = [i * 8 + j for i in range(BLOCK) for j in range(8)
            if (bm_before[i] >> j & 1) != (bm_after[i] >> j & 1)]
    print("  FAIL 位图与造盘时不同，差异位=%s" % diff); bad = 1

# 被删的两个 inode（hi / big）都应当 mode=0、size=0、块号全 0
# 只认"造盘时不存在、现在归零"的那些，不猜具体号
for ino in range(2, 12):
    bmode, _, _ = inode(before, ino)
    amode, asize_i, ablocks = inode(after, ino)
    if bmode == 0 and amode != 0:
        print("  FAIL inode %d 原本空闲，现在被占了" % ino); bad = 1
    if bmode == 0:
        print("inode %d: 造盘时不存在，现在 mode=%d size=%d blocks=%s"
              % (ino, amode, asize_i, ablocks))
print("DISK", "OK" if bad == 0 else "BAD")
sys.exit(bad)
PY
disk_rc=$?
sed 's/^/    /' /tmp/fs3-disk.log

# 4. 重启一次：被删的文件应当真的没了（内存缓存清零后重新从盘上读）
rm -f $QMP $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-fs3b.log 2>&1 &)
sleep 2
ok=0
for i in $(seq 1 40); do
  eip=$(eip_now); eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 120))" ]; then ok=1; break; fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  重启后没等到 kernel_idle"; fail=1; }
type_cmd c l e a r; type_cmd l s; dump
rels=$(screen)
echo "  J. 重启后 ls："
printf '%s\n' "$rels" | grep -E '\| (hello|hi|long|blob|big)' | sed 's/^/    /'

fail=0
[ -n "$rmline" ] || { echo "  FAIL  rm hi 没有成功行"; fail=1; }
n0=$(printf '%s\n' "$rmline" | sed -n 's/.* n=\([0-9]*\)->.*/\1/p')
n1=$(printf '%s\n' "$rmline" | sed -n 's/.*->\([0-9]*\).*/\1/p')
[ -n "$n0" ] && [ -n "$n1" ] && [ "$n1" -lt "$n0" ] \
  || { echo "  FAIL  目录项数没有减少：$rmline"; fail=1; }
printf '%s\n' "$after_ls" | grep -qF '| hi ' \
  && { echo "  FAIL  ls 里还有 hi"; fail=1; }
[ -n "$cat_hi" ] || { echo "  FAIL  cat hi 没有报 no such file"; fail=1; }
printf '%s\n' "$rm_nope" | grep -q 'rc=E1' \
  || { echo "  FAIL  rm nope.txt 没有返回 E1：'$rm_nope'"; fail=1; }
printf '%s\n' "$big_line" | grep -qE 'big +528' \
  || { echo "  FAIL  big 没建成 528 字节：'$big_line'"; fail=1; }
[ -n "$rmbig" ] || { echo "  FAIL  rm big 没有成功行"; fail=1; }
printf '%s\n' "$fsline" | grep -qE 'un=2' \
  || { echo "  FAIL  fs 的 un 不是 2：'$fsline'"; fail=1; }
[ "$disk_rc" = 0 ] || { echo "  FAIL  宿主机读盘校验没过"; fail=1; }
printf '%s\n' "$rels" | grep -qF '| hi ' \
  && { echo "  FAIL  重启后 hi 又出现了（说明只删了内存）"; fail=1; }
printf '%s\n' "$rels" | grep -qF '| big ' \
  && { echo "  FAIL  重启后 big 又出现了"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  mosfs 删除：目录项/inode/位图三处都改了，重启后也没复活"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
