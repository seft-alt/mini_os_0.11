#!/bin/bash
# fd 层：把 mosfs 的 (ino, offset) 收成"一个整数"，并且内核和 ring3 走同一条路。
#
# 判据：
#   A. `fd` 初始为空表（on=0 / dn=0），说明表是按槽位起手就干净的；
#   B. `cat hello.txt` 仍读得出内容 —— cat 已经改走 fd（open→read→close），
#      所以这条同时是老功能的回归和新路径的第一份证据；
#   C. `fd hello.txt` 三行 —— 打开拿到 fd=0、读 16 字节后 off 跟着到 16、
#      关掉 rc=0。off 移动是"fd 真有状态"的直接证据（少这一条就只是文件句柄别名）；
#   D. `exec fdprobe` —— 数据盘里的探针在 ring3 里 open/read/write/close：
#      val=16（读回 16 字节）、ex=16，屏幕上出现真内容 "hello from mosfs"；
#   E. `cat fdout` 读到 "fdwr" —— 用户程序**写**出来的文件，内核侧读得回来：
#      两个方向都打通了，且落盘的是真字节；
#   F. 再跑一次 `fd`：on=0（任务死后整表被收）、dn 全部来自 E 段之前的拒绝，
#      说明"fd 归任务"这条线没漏。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/fd-console.bin
SER=/tmp/fd-serial.log
IMG=/tmp/fd.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

[ -f build/uprog5.bin ] || { echo "  FAIL  build/uprog5.bin 不存在（make 没生成）"; exit 1; }
# 位置参数：img/uprog/bad/elf/big/paged/huge/slow/argv/fd
python3 tools/datadisk.py $IMG build/uprog.bin '' '' '' '' '' '' '' build/uprog5.bin \
  >/tmp/datadisk-fd.log 2>&1 || { echo "  FAIL  造盘失败"; exit 1; }
tail -1 /tmp/datadisk-fd.log | sed 's/^/  /'

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-exec-fd.log 2>&1 &)
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

type_cmd f d; dump
t0=$(screen | grep -oE 'fd s0=[0-9]+ s1=[0-9]+ .* dn=[0-9]+' | head -1)
echo "  A. 初始表：${t0:-（没有输出）}"

type_cmd c l e a r; type_cmd c a t spc h e l l o dot t x t; dump
b=$(rows | grep -x 'hello from mosfs v1' | head -1)
echo "  B. cat 走 fd 读出的第一行：${b:-（没有）}"

type_cmd c l e a r; type_cmd f d spc h e l l o dot t x t; dump
op=$(screen | grep -oE 'fd open rc=0 fd=[0-9]+ ino=[0-9]+ sz=[0-9]+' | head -1)
rd=$(screen | grep -oE 'fd read n=[0-9]+ off=[0-9]+' | head -1)
cl=$(screen | grep -oE 'fd close rc=[0-9E]+' | head -1)
echo "  C. ${op:-（没有打开行）} / ${rd:-（没有读行）} / ${cl:-（没有关行）}"

type_cmd c l e a r; type_cmd e x e c spc f d p r o b e; dump
ex=$(screen | grep -oE 'exec fdprobe rc=0 mag=0x[0-9A-F]{8} val=[0-9]+ ex=[0-9]+' | head -1)
said=$(rows | grep -x 'hello from mosfs' | head -1)
echo "  D. ${ex:-（没有 exec 行）}"
echo "  D2. ring3 read 回来的内容：${said:-（没有）}"

type_cmd c l e a r; type_cmd c a t spc f d o u t; dump
e=$(rows | grep -x 'fdwr' | head -1)
echo "  E. cat 用户程序写的 fdout：${e:-（没有）}"

type_cmd c l e a r; type_cmd f d; dump
t1=$(screen | grep -oE 'fd s0=[0-9]+ s1=[0-9]+ .* dn=[0-9]+' | head -1)
echo "  F. 收工后的表：${t1:-（没有输出）}"

fail=0
printf '%s\n' "$t0" | grep -q ' on=0 ' || { echo "  FAIL  初始表不是空的：'$t0'"; fail=1; }
[ "$b" = "hello from mosfs v1" ] || { echo "  FAIL  cat 没读出发真内容（fd 路径断了？）"; fail=1; }
printf '%s\n' "$op" | grep -q 'fd open rc=0 fd=0 ino=' \
  || { echo "  FAIL  内核侧 open 没成功：'$op'"; fail=1; }
[ "$rd" = "fd read n=16 off=16" ] \
  || { echo "  FAIL  读 16 字节后 off 应当到 16：'$rd'"; fail=1; }
[ "$cl" = "fd close rc=0" ] || { echo "  FAIL  关没成功：'$cl'"; fail=1; }
printf '%s\n' "$ex" | grep -q 'rc=0 mag=0x45584543 val=16 ex=16' \
  || { echo "  FAIL  ring3 的 fd 探针没按约定退出：'$ex'"; fail=1; }
[ "$said" = "hello from mosfs" ] \
  || { echo "  FAIL  屏幕上看不到 ring3 读回来的内容（read 没真读？）"; fail=1; }
[ "$e" = "fdwr" ] || { echo "  FAIL  用户程序写的 fdout 读不回来（写路径没落盘？）"; fail=1; }
printf '%s\n' "$t1" | grep -q ' on=0 ' \
  || { echo "  FAIL  任务跑完 fd 表没归零（fd 归任务这条线漏了）：'$t1'"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  fd 层：内核与 ring3 共用一条路径（open 得 fd=0、read 后 off=16、"
  echo "      ring3 读回 16 字节并写出 fdout、任务死后整表归零）"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
