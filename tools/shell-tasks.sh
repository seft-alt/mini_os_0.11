#!/bin/bash
# `tasks` 命令：每槽位一行，证明"谁在跑 / 还剩几 tick / 一共吃了多少 CPU"都能被看见。
#
# 判定分两层：
#   A 段（内存证据）——直接读 tasks[] 的字段：ticks 是累计 CPU tick，
#      boot 跑完之后至少有一个 worker 的 ticks > 0，且各槽位 ticks 之和 <= 时钟向量计数；
#   B 段（屏幕证据）——真按键敲 `tasks` ⏎，用户页每一行都符合
#      `t<slot> <state> p=<d> s=<d>/<d> c=0x<6 位> r=<d>`。
# 只断言 B 段会漏掉"渲染对了但账算错了"，只断言 A 段会漏掉"账对了但没打出来"。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
# 布局契约：task_t 的 sizeof 与字段偏移从 build/layout.env 读（由 make 从结构体
# 生成，见 kernel/layout.c）。脚本不再自己写 108 / +24 —— 结构体一动，重跑 make
# 就跟着变，不会静默读到错位的内存还报 PASS。
. build/layout.env 2>/dev/null || { echo "  FAIL  build/layout.env 不存在，先 make"; exit 1; }
QMP=/tmp/qmp.sock
CON=/tmp/tasks-console.bin
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {  # 读一个 32 位字（静态/全局变量）
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}
screen() { python3 tools/vga-screen.py "$CON"; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-tasks.log 2>&1 &)
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

# A 段：tasks[] 每个槽位 sizeof(task_t)=$TASK_SIZE 字节，ticks 在 +$TASK_OFF_TICKS
# （这两个数字来自 build/layout.env，等于编译器对 task_t 的答案；改结构体不用改脚本）
tasks_sym=$(sym tasks)
[ -n "$tasks_sym" ] || { echo "  FAIL  nm 找不到 tasks[]"; killall -q -9 qemu-system-i386; exit 1; }
tick_vec=$(python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$(printf '%x' $((0x$(sym hits) + 4 * 0x20)))" \
             | tr -d '\r' | awk '{print $NF}')
sum_ticks=0
ticks_of() {  # 取某个槽位的累计 CPU tick
  local off; off=$(printf '0x%x' $((0x$tasks_sym + TASK_SIZE * $1 + TASK_OFF_TICKS)))
  python3 tools/qemu-qmp.py $QMP "xp /1dw $off" | tr -d '\r' | awk '{print $NF}'
}
t0_a=$(ticks_of 0)
for slot in 0 1 2 3 4; do sum_ticks=$((sum_ticks + $(ticks_of $slot))); done
echo "  A. tasks[].ticks 合计=$sum_ticks（slot 0 = $t0_a），时钟向量=$tick_vec"

# B 段：真按键 `tasks` ⏎（中间等 1.2 秒，正好让 slot 0 的账再涨一段）
python3 tools/qemu-sendkey.py $QMP t a s k s ret >/tmp/keys-tasks.log 2>&1
sleep 1.2
t0_b=$(ticks_of 0)
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null
echo "  --- 用户页 dump ---"
screen | grep -E 't[0-9] | tasks' | head -8

lines=$(screen | grep -oE 't[0-9] (run|ready|sleep|dead|unused) p=[0-9]+ s=[0-9]+/[0-9]+ c=0x[0-9A-F]{6} r=[0-9]+ e=0x[0-9A-F]{6} u=[0-9]+' || true)
n_lines=$(printf '%s\n' "$lines" | grep -c . )
t0_screen=$(printf '%s\n' "$lines" | sed -n 's/^t0 .* c=0x\([0-9A-F]\{6\}\).*/\1/p')
t0_scr_dec=$((0x${t0_screen:-0}))
echo "  B. slot 0 的 c：读内存 $t0_a -> $t0_b，屏幕 0x${t0_screen:-（无）}"

fail=0
[ "$t0_b" -gt "$t0_a" ] 2>/dev/null || { echo "  FAIL  slot 0 的 CPU tick 应当随时间上涨（$t0_a -> $t0_b）"; fail=1; }
[ "$sum_ticks" -le "$tick_vec" ] 2>/dev/null || { echo "  FAIL  tasks ticks 合计($sum_ticks) 不该超过时钟向量计数($tick_vec)"; fail=1; }
if [ -n "${t0_screen:-}" ]; then
  { [ "$t0_scr_dec" -ge "$t0_a" ] && [ "$t0_scr_dec" -le "$t0_b" ]; } \
    || { echo "  FAIL  屏幕上的 c 不在两读之间（$t0_a..$t0_b vs 0x$t0_screen）"; fail=1; }
else
  echo "  FAIL  屏幕上没有 slot 0 那一行"; fail=1
fi
[ "$n_lines" -ge 2 ] || { echo "  FAIL  至少应列出 slot 0 和一个 worker，实际 $n_lines 行"; fail=1; }
if screen | grep -q '> tasks'; then :; else echo "  FAIL  用户页没有回显 '> tasks'"; fail=1; fi
printf '%s\n' "$lines" | grep -qE '^t0 (run|ready)' \
  || { echo "  FAIL  slot 0 应当处在 run/ready"; fail=1; }
printf '%s\n' "$lines" | grep -qE '^t[1-4] (unused|dead|ready|sleep|run) ' \
  || { echo "  FAIL  worker 槽位一条都没列出来"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  tasks：slot 0 的 CPU tick 在涨（$t0_a -> $t0_b），且与屏幕 0x$t0_screen 对得上；$n_lines 行槽位可见"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
