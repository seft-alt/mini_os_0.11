#!/bin/bash
# 参数传递：argc/argv 真的从内核递到了 ring3 吗。
#
# 数据盘里的 "argvp" 是 boot/uprog4.asm 编出来的探针，它做三件事：
#   1. argc >= 2 就打 argv[1]，否则打 argv[0]；
#   2. 把 argc 写进结果区（内核打 val=）；
#   3. exit(argc)（退出码同样印证）。
#
# 判据：
#   A. `exec argvp hello` → rc=0、magic 自证、val=2、ex=2，屏幕上出现 "hello"
#      —— 说明**第 2 个参数**真的到了用户态，不是只有名字；
#   B. `exec argvp`（不带参数）→ val=1、ex=1，屏幕上出现 "argvp"
#      —— 说明 argc 是 1 而不是 0，argv[0] 是程序名（标准习惯）；
#   C. 两趟跑完 jb=0 / asu=0：job 表和地址空间池都没漏。
# 为什么值得单独一条：参数以前根本到不了用户程序，位置错一字节也不会崩，
# 只会"打出来的东西不对"，靠肉眼很容易放过。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/argv-console.bin
SER=/tmp/argv-serial.log
IMG=/tmp/argv.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
# 只取每行的正文（去掉 "row  N| " 前缀），这样 grep -x 才能按整行匹配
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

[ -f build/uprog4.bin ] || { echo "  FAIL  build/uprog4.bin 不存在（make 没生成）"; exit 1; }
# 位置参数：img/uprog/bad/elf/big/paged/huge/slow/argv
python3 tools/datadisk.py $IMG build/uprog.bin '' '' '' '' '' '' build/uprog4.bin \
  >/tmp/datadisk-argv.log 2>&1 || { echo "  FAIL  造盘失败"; exit 1; }
tail -1 /tmp/datadisk-argv.log | sed 's/^/  /'

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-exec-argv.log 2>&1 &)
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

type_cmd e x e c spc a r g v p spc h e l l o; dump
one=$(screen | grep -oE 'exec argvp rc=0 mag=0x[0-9A-F]{8} val=[0-9]+ ex=[0-9]+' | head -1)
echo "  A. ${one:-（没有输出）}"
said=$(rows | grep -x 'hello' | head -1)
echo "  A2. 程序自己打出来的参数：${said:-（没有）}"
l2=$(screen | grep -oE 'exec u=[0-9]+ lh=[0-9]+ jb=[0-9]+ asu=[0-9]+' | head -1)
echo "  A3. 第二行：${l2:-（没有）}"

type_cmd c l e a r; type_cmd e x e c spc a r g v p; dump
zero=$(screen | grep -oE 'exec argvp rc=0 mag=0x[0-9A-F]{8} val=[0-9]+ ex=[0-9]+' | head -1)
echo "  B. 不带参数：${zero:-（没有输出）}"
said0=$(rows | grep -x 'argvp' | head -1)
echo "  B2. 这时打的是 argv[0]：${said0:-（没有）}"

fail=0
[ -n "$one" ] || { echo "  FAIL  没有 exec 输出行"; fail=1; }
printf '%s\n' "$one" | grep -q 'rc=0 mag=0x45584543 val=2 ex=2' \
  || { echo "  FAIL  带一个参数时应 val=2 / ex=2：'$one'"; fail=1; }
[ "$said" = "hello" ] \
  || { echo "  FAIL  没看到 argv[1] 被 ring3 打出来（参数没递到？）"; fail=1; }
printf '%s\n' "$l2" | grep -q 'jb=0 asu=0' \
  || { echo "  FAIL  job 表或地址空间池没归零：'$l2'"; fail=1; }
printf '%s\n' "$zero" | grep -q 'rc=0 mag=0x45584543 val=1 ex=1' \
  || { echo "  FAIL  不带参数时应 val=1 / ex=1：'$zero'"; fail=1; }
[ "$said0" = "argvp" ] \
  || { echo "  FAIL  不带参数时没打出 argv[0]（应是程序名）"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  参数：argv[1] 递到 ring3（val=2/ex=2、打出 hello），"
  echo "      不带参数时 argc=1、argv[0]=程序名，job/地址空间池都归零"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
