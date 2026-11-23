#!/bin/bash
# 每任务一套用户页 + TSS.esp0 跟着任务走：ring3 现在落在**这个任务自己的**栈上。
#
# 要证明的三件事（少一件这一步就等于没做）：
#   1. 空闲时 TSS.esp0 == 默认栈顶（tss_stack + 4096，落在内核映像 .bss 里，
#      判据是 <= __kernel_end，不再写死 0x20000——内核变大后那个阈值会假红）；
#   2. 程序真跑在 ring3（CS=0x23）的那一刻，TSS.esp0 落在任务页区间
#      [0x25000, 0x90000)、页对齐、且 == 某槽位 task_t.esp0（不是默认栈）；
#   3. 跑完之后 TSS.esp0 回到默认栈顶，且物理页用量 `heap pf=` 一分不差。
#
# 探针是 boot/uprog3.asm：一段 ring3 忙等（ring3 期间 IF=0，tick 不会涨，
# 所以只能烧 CPU）。它停留多久就在第 2 条里打出来。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
# 布局契约：sizeof(task_t) 与 esp0 偏移从 build/layout.env 读（make 生成）——
# 这个脚本以前把 108/+92 写死在两处，task_t 一动就会静默读错页。
. build/layout.env 2>/dev/null || { echo "  FAIL  build/layout.env 不存在，先 make"; exit 1; }
QMP=/tmp/qmp.sock
CON=/tmp/userpg-console.bin
SER=/tmp/userpg-serial.log
IMG=/tmp/userpg.img
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
cs_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*CS =\([0-9a-f]*\).*/\1/p'; }
screen() { python3 tools/vga-screen.py "$CON"; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_cmd() { python3 tools/qemu-sendkey.py $QMP "$@" ret >/dev/null 2>&1; sleep 0.9; }
# 读 tasks[slot] 的某个 32 位字段（task_t 布局见 build/layout.env，esp0 在 TASK_OFF_ESP0）
# 注意：QEMU 的 `xp /Nd` 输出的是**十进制**，所以这里统一返回十进制。
task_esp0() {
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$(printf '%x' $((0x$tasks_sym + TASK_SIZE * $1 + TASK_OFF_ESP0)))" \
    | tr -d '\r' | awk '{print $NF}'
}
tss_esp0() {  # TSS 结构的 esp0 在偏移 +4
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$(printf '%x' $((0x$tss_sym + 4)))" \
    | tr -d '\r' | awk '{print $NF}'
}
hex() { printf '0x%x' "$1"; }

[ -f build/uprog3.bin ] || { echo "  FAIL  build/uprog3.bin 不存在（make 应当先编出来）"; exit 1; }
tss_sym=$(sym tss); tasks_sym=$(sym tasks); stack_sym=$(sym tss_stack); end_sym=$(sym __kernel_end)
[ -n "$tss_sym" ] && [ -n "$tasks_sym" ] && [ -n "$stack_sym" ] && [ -n "$end_sym" ] \
  || { echo "  FAIL  nm 找不到 tss / tasks / tss_stack / __kernel_end"; exit 1; }
default_esp0=$(printf '%d' $((0x$stack_sym + 4096)))
echo "  A0. tss=0x$tss_sym tasks=0x$tasks_sym tss_stack=0x$stack_sym"
echo "     默认栈顶（tss_stack+4096）= 0x$(printf '%x' $default_esp0)"
[ "$default_esp0" -le $((0x$end_sym)) ] \
  || { echo "  FAIL  默认栈顶应当落在内核映像内（<= __kernel_end=0x$end_sym），实际 0x$(printf '%x' $default_esp0)"; exit 1; }

python3 tools/datadisk.py $IMG build/uprog.bin '' build/uprog.elf '' '' '' build/uprog3.bin \
  >/tmp/datadisk-userpg.log 2>&1 || { echo "  FAIL  造盘失败"; exit 1; }
sed 's/^/  /' /tmp/datadisk-userpg.log

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-userpg.log 2>&1 &)
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

# ---- A 段：空闲时的两条基线 ----
e0_idle=$(tss_esp0)
pf0=$(type_cmd c l e a r; type_cmd h e a p; dump; screen | grep -oE 'pf=[0-9]+/[0-9]+' | head -1)
e0_slot0=$(task_esp0 0)
echo "  A1. 空闲：tss.esp0=$(hex "$e0_idle")  tasks[0].esp0=$(hex "$e0_slot0")  默认=$(hex "$default_esp0")"
echo "  A2. heap 基线 $pf0"

# ---- B 段：把 ring3 探针放出去，边跑边采现场 ----
# 采样走 tools/qmp-watch.py：一条 QMP 连接里反复读 CS、tss.esp0，
# 外加 5 个槽位的 tasks[i].esp0 —— 必须**同一瞬间**读，
# 因为任务一死槽位立刻 UNUSED，事后再去对表就没得对了。
slot_addrs=""
for slot in 0 1 2 3 4; do
  slot_addrs="$slot_addrs $((0x$tasks_sym + TASK_SIZE * $slot + TASK_OFF_ESP0))"
done
# 注意：QMP 的 unix socket 一次只能挂一个客户端（不设 server,nowait 的多路复用），
# 所以**不能**让 sendkey 与 watch 并发 —— 并发时后到的那个连不上，键根本没发出去
# （实测：本轮 400 次采样 0 次 ring3、屏幕上根本没有 exec 行）。
# 顺序必须是"先敲完键、再开始采样"：探针要在 ring3 里忙等好几秒，采样来得及。
t0=$(date +%s%N)
python3 tools/qemu-sendkey.py $QMP e x e c spc u p r o g 3 ret >/dev/null 2>&1
watch=$(python3 tools/qmp-watch.py $QMP $((0x$tss_sym + 4)) 400 5 20 $slot_addrs)

n_samp=$(printf '%s\n' "$watch" | grep -c '^S ')
n_ring3=$(printf '%s\n' "$watch" | grep -cE '^S 00[0-9A-Fa-f]*3 ')
summary=$(printf '%s\n' "$watch" | grep '^SUMMARY')
ring3_esp0=$(printf '%s' "$summary" | awk '{print $4}')
ring3_esp0=${ring3_esp0:-0}
ring3_line=$(printf '%s\n' "$watch" | grep -E '^S 00[0-9A-Fa-f]*3 ' | head -1)

# 等屏幕上出现 exec 那一行（忙等 400M 次，实测十几秒，给足 60 秒）
line=""
for i in $(seq 1 60); do
  dump
  line=$(screen | grep -oE 'exec uprog3 rc=[0-9E-]+ .*' | head -1)
  [ -n "$line" ] && break
  sleep 1
done
t1=$(date +%s%N)
ms=$(( (t1 - t0) / 1000000 ))
dump
out=$(screen)
echo "  B. 采样 $n_samp 次，其中 ring3（CS&3=3）$n_ring3 次"
echo "     ring3 那一行：$ring3_line"
printf '%s\n' "$out" | grep -E 'exec uprog3|uprog3' | sed 's/^/    /'
echo "     exec uprog3 从按键到出结果约 ${ms}ms"

# ---- C 段：收尾 ----
e0_after=$(tss_esp0)
pf1=$(type_cmd c l e a r; type_cmd h e a p; dump; screen | grep -oE 'pf=[0-9]+/[0-9]+' | head -1)
echo "  C. 收尾：tss.esp0=$(hex "$e0_after")  heap $pf1"
type_cmd c l e a r; type_cmd t a s k s; dump
echo "  --- tasks（e= 是各槽位的进内核栈顶）---"
tasks_out=$(screen)
printf '%s\n' "$tasks_out" | grep -oE 't[0-9] (run|ready|sleep|dead|unused).*' | head -6 | sed 's/^/    /'
t0_line=$(printf '%s\n' "$tasks_out" | grep -oE '^row +[0-9]+\| t0 .*' | head -1)
t0_e=$(printf '%s' "$t0_line" | sed -n 's/.* e=0x\([0-9A-F]*\).*/\1/p')
echo "     slot 0 那行的 e=0x${t0_e:-无}"

killall -q -9 qemu-system-i386 2>/dev/null

fail=0
[ "$e0_idle" = "$e0_slot0" ] \
  || { echo "  FAIL  空闲时 tss.esp0 应当等于 tasks[0].esp0（$e0_idle vs $e0_slot0）"; fail=1; }
[ "$e0_idle" = "$default_esp0" ] \
  || { echo "  FAIL  空闲时 tss.esp0 应当还是默认栈顶（$e0_idle vs $default_esp0）"; fail=1; }
[ "$n_ring3" -ge 1 ] \
  || { echo "  FAIL  整个采样窗口里没见到 CS=0x23（探针没进 ring3 或太短）"; fail=1; }
if [ "$n_ring3" -ge 1 ]; then
  [ "$ring3_esp0" -ge $((0x25000)) ] && [ "$ring3_esp0" -lt $((0x90000)) ] \
    || { echo "  FAIL  ring3 时的 esp0 不在任务页区间：$(hex "$ring3_esp0")"; fail=1; }
  [ $((ring3_esp0 % 4096)) -eq 0 ] \
    || { echo "  FAIL  ring3 时的 esp0 不是页对齐（$(hex "$ring3_esp0")）"; fail=1; }
  [ "$ring3_esp0" != "$default_esp0" ] \
    || { echo "  FAIL  ring3 时还在用全局默认栈（这一步等于没做）"; fail=1; }
  # 同一瞬间读到的 5 个槽位 esp0 里，必须有且只有一个是这个值：
  # 这就是"TSS.esp0 等于当前任务自己的 esp0"的直接证据。
  hit_slot=""
  for f in 1 2 3 4 5; do
    v=$(printf '%s' "$ring3_line" | awk -v i=$((f + 3)) '{print $i}')
    [ -n "$v" ] && [ "$v" = "$ring3_esp0" ] && hit_slot=$((f - 1))
  done
  [ -n "$hit_slot" ] \
    || { echo "  FAIL  ring3 时刻的 tss.esp0 对不上任何槽位的 task_t.esp0：$ring3_line"; fail=1; }
  echo "     ring3 时的 esp0 就是 slot $hit_slot 的 esp0"
fi
printf '%s\n' "$line" | grep -q 'rc=0' \
  || { echo "  FAIL  exec uprog3 没成功：'$line'"; fail=1; }
printf '%s\n' "$line" | grep -q 'mag=0x45584543' \
  || { echo "  FAIL  探针没写指纹：'$line'"; fail=1; }
printf '%s\n' "$line" | grep -q 'ex=9' \
  || { echo "  FAIL  退出码不是 9：'$line'"; fail=1; }
printf '%s\n' "$out" | grep -q 'uprog3:slow ok' \
  || { echo "  FAIL  探针没有写出 uprog3:slow ok"; fail=1; }
[ "$e0_after" = "$default_esp0" ] \
  || { echo "  FAIL  跑完之后 tss.esp0 没回默认栈顶（$(hex "$e0_after")）"; fail=1; }
[ "$pf0" = "$pf1" ] \
  || { echo "  FAIL  物理页没还干净：$pf0 -> $pf1"; fail=1; }
# 屏幕上的 e= 也得对：slot 0 用的永远是默认栈（它不是任务页）
[ -n "${t0_e:-}" ] \
  || { echo "  FAIL  tasks 里没有 slot 0 那行（e= 没打出来）"; fail=1; }
[ $((0x${t0_e:-0})) -eq "$default_esp0" ] 2>/dev/null \
  || { echo "  FAIL  tasks 里 slot 0 的 e=0x${t0_e:-无} 不等于默认栈顶 $(hex "$default_esp0")"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  每任务 esp0：ring3 落在自己的任务页（$(hex "$ring3_esp0")），"
  echo "      空闲时回默认栈（$(hex "$default_esp0")），页全归还"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
