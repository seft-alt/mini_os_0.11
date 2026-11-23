#!/bin/bash
# exec：盘上的字节能不能跑到 ring3。
#
# 数据盘里放的是 build/uprog.bin（boot/uprog.asm 编出来的纯二进制），
# 内核不知道它长什么样，只按 mosfs 的文件读出来、拷进用户页、iret 进 ring3。
#
# 判据：
#   1. 跑完：rc=0，且 magic=0x45584543 —— 这个指纹是**程序自己**写下的，
#      不是内核填的（exec 事先把结果区写成 0xDEAD0000，指纹对不上就报 E5）；
#   2. 算对：val=36（程序把 1..8 加起来），说明代码真的执行了，不是"跳过去没跑"；
#   3. 收干净：跑完之后 pages_taken 回到跑之前的值（两页都还了），
#      且租约没剩下（refs 归零，U/S 收回）；
#   4. 错误分支：`exec nope` 报 E1，不会把内核带崩。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/exec-console.bin
SER=/tmp/exec-serial.log
IMG=/tmp/exec.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  [ -n "$a" ] || { echo 0; return; }
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 1.0; }

[ -f build/uprog.bin ] || { echo "  FAIL  build/uprog.bin 不存在（make 没生成）"; exit 1; }
[ -f build/badprog.bin ] || { echo "  FAIL  build/badprog.bin 不存在（make 没生成）"; exit 1; }
python3 tools/datadisk.py $IMG build/uprog.bin build/badprog.bin >/tmp/datadisk-exec.log 2>&1 \
  || { echo "  FAIL  造盘失败"; exit 1; }
tail -1 /tmp/datadisk-exec.log | sed 's/^/  /'

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-exec.log 2>&1 &)
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

before=$(rd pages_taken)
echo "  A. 跑之前 pages_taken=$before"

type_cmd e x e c spc u p r o g; dump
line=$(screen | grep -oE 'exec uprog rc=0 mag=0x[0-9A-F]{8} val=[0-9]+ ex=[0-9]+ sy=[0-9]+/[0-9]+ t=[0-9]+ d=[0-9]+ n=[0-9]+/[0-9]+' | head -1)
echo "  B. ${line:-（没有输出）}"
said=$(screen | grep -o 'uprog:ring3 ok' | head -1)
echo "  B2. 程序自己打的字：${said:-（没有）}"

after=$(rd pages_taken)
echo "  C. 跑之后 pages_taken=$after（应回到 $before）"

type_cmd c l e a r; type_cmd e x e c spc n o p e; dump
bad=$(screen | grep -oE 'exec nope rc=E[0-9]' | head -1)
echo "  D. 错误分支：${bad:-（没有输出）}"

# E：负向程序（越界指针 + 不写指纹）。它必须被两道关各拦一次：
#   门拒绝解引用（sy 里的 1 次 denied），exec 读回哨兵 → E5。
type_cmd c l e a r; type_cmd e x e c spc b a d; dump
evil=$(screen | grep -oE 'exec bad rc=E[0-9] mag=0x[0-9A-F]{8} val=[0-9]+ ex=[0-9]+ sy=[0-9]+/[0-9]+ t=[0-9]+ d=[0-9]+ n=[0-9]+/[0-9]+' | head -1)
echo "  E. 越界指针 + 无指纹：${evil:-（没有输出）}"
after2=$(rd pages_taken)
echo "  E2. 跑之后 pages_taken=$after2（应仍是 $before）"

fail=0
[ -n "$line" ] || { echo "  FAIL  没有 exec 输出行"; fail=1; }
printf '%s\n' "$line" | grep -q 'rc=0 mag=0x45584543 val=36' \
  || { echo "  FAIL  指纹或结果不对：'$line'"; fail=1; }
printf '%s\n' "$line" | grep -q 'ex=36' \
  || { echo "  FAIL  exit 退出码不对（sys_exit 没生效）：'$line'"; fail=1; }
printf '%s\n' "$line" | grep -qE 'sy=[1-9][0-9]*/0' \
  || { echo "  FAIL  syscall 账不对（应 >=1 次且 0 次被拒）：'$line'"; fail=1; }
printf '%s\n' "$line" | grep -qE 't=[1-9][0-9]*' \
  || { echo "  FAIL  没跑在任务里（t=0 说明任务没建起来）：'$line'"; fail=1; }
printf '%s\n' "$line" | grep -q 'd=0' \
  || { echo "  FAIL  死因应是 0（入口 return）——dtor 没被交接：'$line'"; fail=1; }
[ "$said" = "uprog:ring3 ok" ] \
  || { echo "  FAIL  ring3 的 write 没打到屏幕上（sy=write 没生效）"; fail=1; }
[ "$after" = "$before" ] || { echo "  FAIL  页没还干净（$before -> $after）"; fail=1; }
[ "$bad" = "exec nope rc=E1" ] || { echo "  FAIL  错误分支应报 E1（找不到文件），实际 '$bad'"; fail=1; }
printf '%s\n' "$evil" | grep -q 'rc=E5 mag=0xDEAD0000' \
  || { echo "  FAIL  越界指针那趟应报 E5 且指纹仍是哨兵：'$evil'"; fail=1; }
printf '%s\n' "$evil" | grep -q 'ex=7' \
  || { echo "  FAIL  越界指针那趟的 exit 退出码应是 7：'$evil'"; fail=1; }
printf '%s\n' "$evil" | grep -qE 'sy=2/[1-9]' \
  || { echo "  FAIL  越界指针必须被记成一次 denied（应 sy=2/1）：'$evil'"; fail=1; }
[ "$after2" = "$before" ] || { echo "  FAIL  负向那趟的页也没还干净（$before -> $after2）"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  exec：盘上的程序在 ring3 跑完（magic 自证、val=36、write 出声、exit 36）、"
  echo "      两页收干净、E1 分支不崩、越界指针被门拦成 E5 + sy=2/1"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
