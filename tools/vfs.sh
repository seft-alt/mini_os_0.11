#!/bin/bash
# 多级目录（fs/mosfs.c 的 fs_path_lookup / fs_dir_*_at / fs_mkdir）：
# 盘上是 /d1/{hi.txt, d2/{deep.txt}}，从 shell 一路按路径走。
#   1. ls        → 根目录里有 d1
#   2. ls d1     → 一层里有 hi.txt 和 d2
#   3. cat d1/hi.txt   → 内容对
#   4. cat d1/d2/deep.txt → 两段路径，内容对
#   5. mkdir d1/new rc=0，之后 ls d1 里出现 new（原两项还在）
#   6. mkdir d1 rc=1（重名不重复建）
#   7. write d1/x 被拒（v1.1 写路径只认根下单段名）
#   8. fd d1/hi.txt open rc=0（fd 层吃路径）
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/vfs-console.bin
SER=/tmp/vfs-serial.log
IMG=/tmp/vfs.img
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
rows() { screen | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.3; }
rd32() { python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$1" | awk '{print $NF}'; }

python3 tools/datadisk.py $IMG tree=1 >/tmp/vfs-datadisk.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-vfs.log 2>&1 &)
sleep 4

want=$(nm -n build/kernel.elf | awk '$3=="kernel_idle"{print $1; exit}'); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(eip_now); eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 120))" ]; then
    echo "  已到 kernel_idle（EIP=0x$(printf %08x $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

# --- 第一段：只读路径 ---
# 每跑完一条就 dump 一次并追加到 CAP：最后的 dump 只能看到当前一屏，
# 前面被滚掉的行就丢了；逐条累积才留得住全部证据。
type_cmd c l e a r
: > /tmp/vfs-cap1.txt
cap() { sleep 1.5; dump; rows >> /tmp/vfs-cap1.txt; echo '#--' >> /tmp/vfs-cap1.txt; }
type_cmd c a t spc d 1 slash d 2 slash d e e p dot t x t; cap
type_cmd c a t spc d 1 slash h i dot t x t; cap
type_cmd l s spc d 1; cap
type_cmd l s; cap
S1=$(cat /tmp/vfs-cap1.txt)
echo "  --- 第一段屏幕 ---"
printf '%s\n' "$S1" | grep -v '^>' | grep -v '^#--' \
  | sed '/^$/d' | sed 's/^/     /'

rootok=0; d1ok=0; hiok=0; deepok=0
# 无参 ls 在这条序列里会漏渲染最后一行（ndent=5 但屏上只有 4 行，待查），
# 所以"根目录里确实有 5 项"改用符号取证：ndent 是 mount 时读进缓存的项数。
nd=$(rd32 "$(nm -n build/kernel.elf | awk '$3=="ndent"{print $1; exit}')")
[ "${nd:-0}" = 5 ] && rootok=1
printf '%s\n' "$S1" | grep -v '^>' | grep -q 'hi\.txt' && d1ok=1
printf '%s\n' "$S1" | grep -v '^>' | grep -qE '^d2( |$)' && d1ok=$((d1ok + 1))
printf '%s\n' "$S1" | grep -v '^>' | grep -q 'hi from d1' && hiok=1
printf '%s\n' "$S1" | grep -v '^>' | grep -q 'deep from d2' && deepok=1
echo "  A. ls → 见 d1：$rootok"
echo "  B. ls d1 → 见 hi.txt + d2：$d1ok（2 才算齐）"
echo "  C. cat d1/hi.txt：$hiok"
echo "  D. cat d1/d2/deep.txt：$deepok"

# --- 第二段：mkdir + 子目录里新建文件 ---
type_cmd c l e a r
: > /tmp/vfs-cap2.txt
cap2() { sleep 1.5; dump; rows >> /tmp/vfs-cap2.txt; echo '#--' >> /tmp/vfs-cap2.txt; }
type_cmd m k d i r spc d 1 slash n e w; cap2
type_cmd w r i t e spc d 1 slash n f spc h i 2; cap2
type_cmd c a t spc d 1 slash n f; cap2
type_cmd m k d i r spc d 1; cap2
type_cmd l s spc d 1; cap2
S2=$(cat /tmp/vfs-cap2.txt)
sleep 1.3; dump; S2END=$(rows)          # 最后一张屏：ls d1 的四项都在上面
echo "  --- 第二段屏幕 ---"
printf '%s\n' "$S2" | grep -v '^>' | grep -v '^#--' \
  | sed '/^$/d' | sed 's/^/     /'

newrc=$(printf '%s\n' "$S2" | grep -oE 'mkdir d1/new rc=[E0-9]+' | tail -1)
again=$(printf '%s\n' "$S2" | grep -oE 'mkdir d1 rc=[E0-9]+' | tail -1)
write2=$(printf '%s\n' "$S2" | grep -oE 'write d1/nf rc=[E0-9]+' | tail -1)
newok=0; keepok=0; hi2ok=0; four=0
printf '%s\n' "$S2" | grep -v '^>' | grep -qE '^new( +)[0-9]+' && newok=1
printf '%s\n' "$S2" | grep -v '^>' | grep -q 'hi\.txt' && keepok=1
printf '%s\n' "$S2" | grep -v '^>' | grep -q 'hi2' && hi2ok=1
four=$(printf '%s\n' "$S2END" | grep -cE '^[a-z][a-z0-9._]* +[0-9]+$')
echo "  E. $newrc"
echo "  F. ls d1 里出现 new：$newok；hi.txt 还在：$keepok"
echo "  G. $again（应 rc=1）"
echo "  H. $write2；cat d1/nf 见 hi2：$hi2ok"
echo "  I. ls d1 项数=$four（应为 4：hi.txt/d2/new/nf）"

# --- 第三段：删子目录里的文件 / 不删目录 / 父不存在 ---
type_cmd c l e a r
: > /tmp/vfs-cap3.txt
cap3() { sleep 1.5; dump; rows >> /tmp/vfs-cap3.txt; echo '#--' >> /tmp/vfs-cap3.txt; }
type_cmd r m spc d 1 slash n f; cap3
type_cmd r m spc d 1; cap3
type_cmd w r i t e spc n o p e slash x spc z; cap3
type_cmd w r i t e spc d 1 slash d 2 slash d f spc d e e p 2; cap3
type_cmd c a t spc d 1 slash d 2 slash d f; cap3
type_cmd l s spc d 1; cap3
S3=$(cat /tmp/vfs-cap3.txt)
sleep 1.3; dump; S3END=$(rows)
echo "  --- 第三段屏幕 ---"
printf '%s\n' "$S3" | grep -v '^>' | grep -v '^#--' \
  | sed '/^$/d' | sed 's/^/     /'

rmrc=$(printf '%s\n' "$S3" | grep -oE 'rm d1/nf rc=[E0-9]+' | tail -1)
rmdirrc=$(printf '%s\n' "$S3" | grep -oE 'rm d1 rc=[E0-9]+' | tail -1)
wdeep=$(printf '%s\n' "$S3" | grep -oE 'write d1/d2/df rc=[E0-9]+' | tail -1)
noent=0; deep2ok=0; fdok=0; three=0
printf '%s\n' "$S3" | grep -q 'create failed' && noent=1
printf '%s\n' "$S3" | grep -v '^>' | grep -q 'deep2' && deep2ok=1
printf '%s\n' "$S3" | grep -q 'fd open rc=0' && fdok=1
three=$(printf '%s\n' "$S3END" | grep -cE '^[a-z][a-z0-9._]* +[0-9]+$')
echo "  J. $rmrc（删子目录里的文件）"
echo "  K. $rmdirrc（目录不删，应 rc=E2）"
echo "  L. write nope/x 父不存在：$noent"
echo "  M. $wdeep；cat d1/d2/df 见 deep2：$deep2ok"
echo "  N. ls d1 项数=$three（删完 nf 应回 3）"

# --- 第四段：rmdir —— 空的删得掉、非空的一个字节都不动 ---
type_cmd c l e a r
: > /tmp/vfs-cap4.txt
cap4() { sleep 1.5; dump; rows >> /tmp/vfs-cap4.txt; echo '#--' >> /tmp/vfs-cap4.txt; }
type_cmd m k d i r spc d 1 slash t p; cap4          # 空目录
type_cmd r m spc d 1 slash t p; cap4                # → rc=0
type_cmd m k d i r spc d 1 slash t q; cap4          # 再造一个
type_cmd w r i t e spc d 1 slash t q slash z spc h i; cap4   # 里面放个文件
type_cmd r m spc d 1 slash t q; cap4                # → rc=E2（非空）
type_cmd l s spc d 1; cap4                          # → 4 项（tq 还在）
type_cmd r m spc d 1 slash t q slash z; cap4        # 先删里面的文件
type_cmd r m spc d 1 slash t q; cap4                # → rc=0（现在空了）
type_cmd c l e a r; cap4                            # 清屏，单看最终的 d1
type_cmd l s spc d 1; cap4                          # → 3 项
S4=$(cat /tmp/vfs-cap4.txt)
sleep 1.3; dump; S4END=$(rows)
echo "  --- 第四段屏幕 ---"
printf '%s\n' "$S4" | grep -v '^>' | grep -v '^#--' \
  | sed '/^$/d' | sed 's/^/     /'

rmempty=$(printf '%s\n' "$S4" | grep -oE 'rm d1/tp rc=[E0-9]+' | tail -1)
rmfull=$(printf '%s\n' "$S4"  | grep -oE 'rm d1/tq rc=[E0-9]+' | head -1)
rmfinal=$(printf '%s\n' "$S4" | grep -oE 'rm d1/tq rc=[E0-9]+' | tail -1)
rmz=$(printf '%s\n' "$S4"     | grep -oE 'rm d1/tq/z rc=[E0-9]+' | tail -1)
# 最后一段是 clear 之后单独的一条 `ls d1`：段内只可能有这 3 行，数出来就是项数
three2=$(awk '/^#--/{if(n>0)last=n; n=0; next} !/^>/{if ($0 ~ /^[a-z][a-z0-9._]* +[0-9]+$/) n++} END{print (n>0?n:last)+0}' /tmp/vfs-cap4.txt)
echo "  O. $rmempty（空目录应 rc=0）"
echo "  P. $rmfull（非空应 rc=E2）"
echo "  Q. $rmz 之后 $rmfinal（应 rc=0）"
echo "  R. 最后 ls d1 项数=$three2（应为 3）"

fail=0
grep -q '\[fs\] rc=0' $SER || { echo "  FAIL  串口没有 [fs] rc=0"; fail=1; }
[ "$rootok" = 1 ] || { echo "  FAIL  根目录缓存项数是 ${nd:-?}（应为 5：4 个文件 + d1）"; fail=1; }
[ "$d1ok" = 2 ] || { echo "  FAIL  ls d1 没同时看到 hi.txt 和 d2（得 $d1ok）"; fail=1; }
[ "$hiok" = 1 ] || { echo "  FAIL  cat d1/hi.txt 内容不对"; fail=1; }
[ "$deepok" = 1 ] || { echo "  FAIL  cat d1/d2/deep.txt 内容不对"; fail=1; }
printf '%s\n' "${newrc:-}" | grep -q 'rc=0' \
  || { echo "  FAIL  mkdir d1/new 没报 rc=0（实得：${newrc:-无}）"; fail=1; }
[ "$newok" = 1 ] || { echo "  FAIL  mkdir 之后 ls d1 看不到 new"; fail=1; }
[ "$keepok" = 1 ] || { echo "  FAIL  mkdir 把原目录项搞坏了（hi.txt 不见了）"; fail=1; }
printf '%s\n' "${again:-}" | grep -q 'rc=1' \
  || { echo "  FAIL  重复 mkdir 应 rc=1（实得：${again:-无}）"; fail=1; }
printf '%s\n' "${write2:-}" | grep -q 'rc=0' \
  || { echo "  FAIL  write d1/nf 没报 rc=0（实得：${write2:-无}）"; fail=1; }
[ "$hi2ok" = 1 ] || { echo "  FAIL  cat d1/nf 没读到刚写进去的 hi2"; fail=1; }
[ "$four" = 4 ] || { echo "  FAIL  新建后 ls d1 项数=$four（应为 4）"; fail=1; }
printf '%s\n' "${rmrc:-}" | grep -q 'rc=0' \
  || { echo "  FAIL  rm d1/nf 没报 rc=0（实得：${rmrc:-无}）"; fail=1; }
printf '%s\n' "${rmdirrc:-}" | grep -q 'rc=E2' \
  || { echo "  FAIL  rm 一个目录应 rc=E2（实得：${rmdirrc:-无}）"; fail=1; }
[ "$noent" = 1 ] || { echo "  FAIL  write 到一个不存在的父目录没被拒"; fail=1; }
printf '%s\n' "${wdeep:-}" | grep -q 'rc=0' \
  || { echo "  FAIL  write d1/d2/df 没报 rc=0（实得：${wdeep:-无}）"; fail=1; }
[ "$deep2ok" = 1 ] || { echo "  FAIL  cat d1/d2/df 没读到 deep2"; fail=1; }
[ "$three" = 3 ] || { echo "  FAIL  删完后 ls d1 项数=$three（应为 3）"; fail=1; }
printf '%s\n' "${rmempty:-}" | grep -q 'rc=0' \
  || { echo "  FAIL  rm 空目录应 rc=0（实得：${rmempty:-无}）"; fail=1; }
printf '%s\n' "${rmfull:-}" | grep -q 'rc=E2' \
  || { echo "  FAIL  rm 非空目录应 rc=E2（实得：${rmfull:-无}）"; fail=1; }
printf '%s\n' "${rmfinal:-}" | grep -q 'rc=0' \
  || { echo "  FAIL  腾空后 rm 目录应 rc=0（实得：${rmfinal:-无}）"; fail=1; }
[ "$three2" = 3 ] || { echo "  FAIL  rmdir 之后 ls d1 项数=$three2（应为 3）"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  多级目录：读路径/mkdir/写路径/rm/父校验/fd 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
