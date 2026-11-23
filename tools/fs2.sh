#!/bin/bash
# mosfs 写路径：盘上的东西能不能被"改"，而且真的落在盘上。
#
# 为什么不能只信 cat：cat 读的可能是内核自己那一块缓存（blk），
# 内存里对、盘上没写，屏幕照样正确。所以判据分三层：
#   1. 接口层：write 报 rc=0、sz 逐次增长、ls 的大小跟着变；
#   2. 内容层：cat 出来的是新写的文本（新建 / 追加都在）；
#   3. 落盘层（最硬的一条）：关掉 QEMU 之后，在宿主机上直接读 /tmp/fs.img
#      的字节——数据块里有那段文本、inode 的 size/块号对得上、位图里那些块
#      的 bit 都置了位。这一层过了，才叫"写进去"。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/fs2-console.bin
SER=/tmp/fs2-serial.log
IMG=/tmp/fs2.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 0.9; }

python3 tools/datadisk.py $IMG build/uprog.bin >/tmp/datadisk2.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }
sed 's/^/  /' /tmp/datadisk2.log

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-fs2.log 2>&1 &)
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

# 1. 新建一个文件：hi = "abcdef"
type_cmd c l e a r; type_cmd w r i t e spc h i spc a b c d e f; dump
w1=$(screen | grep -oE 'write hi rc=0 .*' | head -1)
echo "  B. $w1"

# 2. cat 出来
type_cmd c l e a r; type_cmd c a t spc h i; dump
c1=$(screen)
echo "  C. cat hi（第一次）：$(printf '%s\n' "$c1" | grep -oE 'abcdef' | head -1)"

# 3. 追加一行：hi = "abcdef\nghi\n"
type_cmd c l e a r; type_cmd w r i t e spc h i spc g h i; dump
w2=$(screen | grep -oE 'write hi rc=0 .*' | head -1)
echo "  D. $w2"

# 4. cat 出来两行
type_cmd c l e a r; type_cmd c a t spc h i; dump
c2=$(screen)
echo "  E. cat hi（第二次）："
printf '%s\n' "$c2" | grep -oE 'abcdef|ghi' | sed 's/^/    /'
type_cmd c l e a r; type_cmd l s; dump
ls2=$(screen | grep -E 'hi +[0-9]+')
echo "  F. ls: $ls2"

# 4.5 单个参数超 23 字符：整行拒掉（以前是静默截断，会写出"少了一半"的内容）
LONG=0123456789012345678901234
keys="w r i t e spc h i spc"
for ((k = 0; k < ${#LONG}; k++)); do keys="$keys ${LONG:$k:1}"; done
type_cmd $keys; dump
long_out=$(screen | grep -oE 'arg too long.*' | head -1)
echo "  F2. $long_out"

#    两个 23 位数字串 -> 拼接后 23+1+23+'\n' = 48 字节；11 次 = 528 字节，
#    必然落到第二个数据块（第一块只有 512 字节）。
T1=01234567890123456789012
T2=abcdefghijklmnopqrstuvw
for i in $(seq 1 11); do
  keys="w r i t e spc b i g spc"
  for ((k = 0; k < ${#T1}; k++)); do keys="$keys ${T1:$k:1}"; done
  keys="$keys spc"
  for ((k = 0; k < ${#T2}; k++)); do keys="$keys ${T2:$k:1}"; done
  type_cmd $keys >/dev/null
done
type_cmd c l e a r; type_cmd w r i t e spc b i g; type_cmd f s; dump
fsline=$(screen | grep -oE 'fs ok=0x[0-9] b=[0-9]+ i=[0-9]+ rd=[0-9]+ er=[0-9]+ wr=[0-9]+ cr=[0-9]+' | head -1)
echo "  G. $fsline"
type_cmd c l e a r; type_cmd l s; dump
lsbig=$(screen | grep -E 'big +[0-9]+')
echo "  H. ls: $lsbig"

# 6. 落盘层：关 QEMU 之后直接读盘
killall -q -9 qemu-system-i386 2>/dev/null; sleep 1
python3 - "$IMG" <<'PY' > /tmp/fs2-disk.log 2>&1
import sys
BLOCK = 512
img = open(sys.argv[1], "rb").read()

def u32(off):
    return int.from_bytes(img[off:off + 4], "little")

# 超级块在 LBA 1
sb = BLOCK
inode_start = u32(sb + 20)
per_blk = u32(sb + 24)
bitmap = u32(sb + 28)
root = u32(sb + 16)

def inode(ino):
    off = inode_start * BLOCK + (ino // per_blk) * BLOCK + (ino % per_blk) * 32
    mode = int.from_bytes(img[off:off + 2], "little")
    size = int.from_bytes(img[off + 4:off + 8], "little")
    blocks = [u32(off + 8 + 4 * j) for j in range(4)]
    return mode, size, blocks

mode, rsize, rblocks = inode(root)
names = {}
for i in range(rsize // 16):
    eo = rblocks[0] * BLOCK + i * 16
    nm = img[eo:eo + 12].split(b"\0")[0].decode("latin1")
    names[nm] = u32(eo + 12)

print("root mode=%d size=%d names=%s" % (mode, rsize, sorted(names)))

def bit_set(b):
    return bool(img[bitmap * BLOCK + b // 8] & (1 << (b % 8)))

bad = 0
for nm, expect in (("hi", b"abcdef\nghi\n"), ("big", None)):
    ino = names.get(nm)
    if ino is None:
        print("MISS %s 不在目录项里" % nm); bad = 1; continue
    m, size, blocks = inode(ino)
    used = [b for b in blocks if b]
    data = b"".join(img[b * BLOCK:(b + 1) * BLOCK] for b in used)[:size]
    print("%s ino=%d mode=%d size=%d blocks=%s bits=%s"
          % (nm, ino, m, size, used, [bit_set(b) for b in used]))
    if expect is not None:
        print("  content=%r expect=%r match=%s"
              % (data, expect, data == expect))
        if data != expect:
            bad = 1
    if not used or any(not bit_set(b) for b in used):
        print("  FAIL 位图没有把这些块置位"); bad = 1
    if size != len(data):
        print("  FAIL size 与数据长度不符"); bad = 1
    if nm == "big" and size <= 512:
        print("  FAIL big 只有 %d 字节，没跨过一块" % size); bad = 1

# hi 应该是 7 + 4 = 11 字节，且只用一块
if names.get("hi") is not None:
    m, size, blocks = inode(names["hi"])
    if size != 11 or len([b for b in blocks if b]) != 1:
        print("  FAIL hi size=%d blocks=%s（应为 11 / 1 块）" % (size, blocks)); bad = 1

print("DISK", "OK" if bad == 0 else "BAD")
sys.exit(bad)
PY
disk_rc=$?
sed 's/^/    /' /tmp/fs2-disk.log

fail=0
[ -n "$w1" ] || { echo "  FAIL  write hi 没有输出 rc=0"; fail=1; }
printf '%s\n' "$c1" | grep -q 'abcdef' || { echo "  FAIL  cat hi 第一次没读到 abcdef"; fail=1; }
printf '%s\n' "$w2" | grep -q 'sz=11' || { echo "  FAIL  第二次 write 之后 sz 不是 11"; fail=1; }
hits=$(printf '%s\n' "$c2" | grep -cE 'abcdef|ghi')
[ "$hits" -eq 2 ] || { echo "  FAIL  cat hi 第二次只命中 $hits 行（应为 2）"; fail=1; }
printf '%s\n' "$ls2" | grep -qE 'hi +11$' || { echo "  FAIL  ls 里 hi 大小不是 11：'$ls2'"; fail=1; }
printf '%s\n' "$long_out" | grep -q 'arg too long' \
  || { echo "  FAIL  超长参数没有被拒（以前是静默截断）"; fail=1; }
[ -n "$fsline" ] || { echo "  FAIL  fs 命令没有输出"; fail=1; }
printf '%s\n' "$lsbig" | grep -qE 'big +528$' \
  || { echo "  FAIL  ls 里 big 大小不是 528：'$lsbig'"; fail=1; }
grep -q 'DISK OK' /tmp/fs2-disk.log \
  || { echo "  FAIL  宿主机读盘校验没过（见上面 DISK 那行）"; fail=1; }
[ "$disk_rc" = 0 ] || fail=1

if [ "$fail" = 0 ]; then
  echo "PASS  mosfs 写路径：新建/追加/跨块 + 关掉 QEMU 后从盘字节读回来 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
[ "$fail" = 0 ]
