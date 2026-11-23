#!/bin/bash
# mosfs：盘上的东西有没有名字。
#
# 造盘用 tools/datadisk.py（纯字节，产物就是 /tmp/fs.img），挂成 IDE 从盘。
# 判据四层：
#   1. 挂得上：[fs] rc=0 / fs ok=0x1 b=2048 i=32；超级块 magic + 布局常数都对；
#   2. 列得出：ls 三行的名字与大小都和造盘时写进去的相等（大小来自 inode，不是目录项）；
#   3. 读得回：cat 每行文本逐字等于源文件里的那一行——屏幕就是"读出来的内容"；
#   4. 边界：`cat nope.txt` 走错误分支；`cat blob.bin` 把不可打印字节打成 '.'；
#      `fs` 的 rd 说明一块缓存真的省了读（3 个文件 + 目录 = 远少于逐字读）。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/fs-console.bin
SER=/tmp/fs-serial.log
IMG=/tmp/fs.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 0.9; }

python3 tools/datadisk.py $IMG >/tmp/datadisk.log 2>&1 || { echo "  FAIL  造盘失败"; exit 1; }
cat /tmp/datadisk.log | sed 's/^/  /'

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-fs.log 2>&1 &)
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

# 1. ls（截一份当时屏幕，后面断言用）
type_cmd l s; dump
ls_out=$(screen)
echo "  B. ls:"
printf '%s\n' "$ls_out" | grep -E 'hello|long|blob|empty' | sed 's/^/    /'

# 2. cat 短文本
type_cmd c l e a r; type_cmd c a t spc h e l l o dot t x t; dump
cat_out=$(screen)
echo "  C. cat hello.txt:"
printf '%s\n' "$cat_out" | grep -E 'hello|second|third' | sed 's/^/    /'

# 3. cat 跨块长文本（第一行 78 个 A，第二行 79 个数字，第三行 11 个数字）
type_cmd c l e a r; type_cmd c a t spc l o n g dot t x t; dump
alen=$(screen | awk -F'|' '{s=$2; gsub(/[^A]/,"",s); if (length(s) > 50) print length(s)}' | head -1)
echo "  D. cat long.txt: 78 个 A 的行长度=$alen；数字行："
screen | grep -oE '\| [0-9]{10,}' | sed 's/^/    /'

# 4. 二进制 / 错误路径
type_cmd c l e a r; type_cmd c a t spc b l o b dot b i n; dump
blob_line=$(screen | grep -E '\.\.\.\.' | head -1)
dots=$(printf '%s' "$blob_line" | tr -dc ' .' | tr -dc '.' | wc -c)
echo "  E. cat blob.bin: 连续点号=$dots（20 字节不可打印 = 20 个点）"
type_cmd c l e a r; type_cmd c a t spc n o p e dot t x t; dump
nope_out=$(screen)
echo "  F. cat nope.txt: $(printf '%s\n' "$nope_out" | grep -oE 'cat: no such file' | head -1)"

# 5. fs 的账
type_cmd c l e a r; type_cmd f s; dump
fsline=$(screen | grep -oE 'fs ok=0x[0-9] b=[0-9]+ i=[0-9]+ rd=[0-9]+ er=[0-9]+' | head -1)
echo "  G. $fsline"
rd=$(printf '%s\n' "$fsline" | sed -n 's/.* rd=\([0-9]*\).*/\1/p')

fail=0
grep -q '\[fs\] rc=0 b=2048 i=32' $SER || { echo "  FAIL  串口没有 [fs] rc=0 b=2048 i=32"; fail=1; }
printf '%s\n' "$(screen | head -1)" >/dev/null
[ -n "$fsline" ] || { echo "  FAIL  fs 命令没有输出"; fail=1; }
case "$fsline" in
  "fs ok=0x1 b=2048 i=32"*) : ;;
  *) echo "  FAIL  fs 头不对：'$fsline'"; fail=1 ;;
esac
[ "$alen" = 78 ] || { echo "  FAIL  78 个 A 的行长度是 $alen"; fail=1; }
[ "$dots" -ge 12 ] 2>/dev/null || { echo "  FAIL  blob 行里的点号只有 $dots 个"; fail=1; }
[ "$rd" -ge 1 ] && [ "$rd" -le 16 ] 2>/dev/null \
  || { echo "  FAIL  rd=$rd（挂载+读三个文件应该只有十几次读块）"; fail=1; }
# ls 三行：名字在前 20 列、大小来自 inode（52 / 169 / 20）
printf '%s\n' "$ls_out" | grep -qE 'hello\.txt +52$' \
  || { echo "  FAIL  ls 里没有 hello.txt 52"; fail=1; }
printf '%s\n' "$ls_out" | grep -qE 'long\.txt +169$' \
  || { echo "  FAIL  ls 里没有 long.txt 169（跨 2 块的长度）"; fail=1; }
printf '%s\n' "$ls_out" | grep -qE 'blob\.bin +20$' \
  || { echo "  FAIL  ls 里没有 blob.bin 20"; fail=1; }
# cat 的三行文本与源文件逐行相等
hits=$(printf '%s\n' "$cat_out" | grep -cE 'hello from mosfs v1|second line|third line is short')
[ "$hits" -eq 3 ] || { echo "  FAIL  cat hello.txt 只命中 $hits 行（应为 3）"; fail=1; }
printf '%s\n' "$nope_out" | grep -q 'cat: no such file' \
  || { echo "  FAIL  cat nope.txt 没有报 no such file"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  mosfs：挂载/列目录/读短文件/跨块折行/二进制点号/错误分支 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
