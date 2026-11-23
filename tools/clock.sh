#!/bin/bash
# 两路时间对齐：tick 走时 + RTC 校准，谁负责什么、错了能不能被拉回来。
#
# 判据：
#   A. 开机锚点对得上：`date` 的 epoch 与宿主机 UTC 相差 ≤180s；
#   B. 走时是 tick 驱动的：等 12 秒再 `date`，epoch 涨了，且不会比真时间跑得还快
#      （tick 只会慢，不会凭空多走）；
#   C. `clock` 里的 ms 是 10ms 一档（100Hz 的量化，能看出走时粒度）；
#   D. `clock sync` 真的能校准：rc=0、d 与"宿主已过时间 - 内核已过时间"对得上；
#   E. 校准后与宿主机差 ≤180s，且单次修正不超过封顶（60s）；
#   F. 校准后的年月日与它自己给的 epoch 自洽（GNU date 复核）；
#   G. `clock skew 20`（把基准故意挪歪 20s）之后，`clock sync` 必须把这 20s
#      拉回来（d≈+20、rc=0）——这条是**确定**的，不依赖漂移是否恰好发生；
#   H. `clock skew 90` 超出封顶：校准必须拒绝（r 加 1、epoch 仍歪着），
#      再用 `clock skew 0` 把误差清零，校准回到 rc=0。
#
# 为什么值得单独一条：两路时间都"看起来在走"，一个是绝对、一个是相对；
# 混用的症状是"日志时间前后颠倒"或"运行时长算错"，而且不会崩。
#
# 一条实测约束（判据阈值的由来）：这台调试机没有 /dev/kvm，QEMU 跑的是 TCG。
# `sleep 12` 在宿主上实测可能真过 17s（两次 `date -u +%s` 之差就是 17），
# 而同一段时间内核 tick 只走了 11s。所以**任何把"sleep 的标称时长"当基准的
# 判据都会红**：D 段的 `hdelta` 含两轮打字 + 两次抓屏，G 段的 `d` 还要叠上
# "测量窗口里又落后的那几秒"。这里的做法是保留方向性判据（d 为正、被拒、
# 拉回），把"校准真的生效"交给 E/G 里**前后对比**（left 从 behind 降到 ≤5s），
# 而不是硬比一个理想值。历史上 4/2/3 这几个阈值就是这么被定成 10/范围/5 的。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/clock-console.bin
SER=/tmp/clock-serial.log
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
type_text() {
  local s="$1" keys=() c
  for ((i = 0; i < ${#s}; i++)); do
    c="${s:i:1}"
    [ "$c" = " " ] && c=spc
    keys+=("$c")
  done
  python3 tools/qemu-sendkey.py $QMP "${keys[@]}" ret >/dev/null 2>&1
  sleep 0.6
}
run() { type_text "clear"; type_text "$1"; sleep 0.6; dump; }
date_ep()  { rows | grep -oE 'ep=[0-9]+' | head -1 | sed -E 's/ep=//'; }
date_txt() { rows | grep -oE 'date [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} [A-Za-z]{3}' | head -1 | sed 's/date //'; }
# 取 clock 行里的某个十进制字段：前面加空格定位，才不会把 `ms=` 当成 `s=`
fld() { rows | grep -oE " $1=[+-]?[0-9]+" | head -1 | sed -E "s/ $1=//"; }
abs() { [ "$1" -lt 0 ] && echo $((- $1)) || echo "$1"; }

fail=0
killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-clock.log 2>&1 &)
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

# ---- A. 开机锚点 ----
run "date"
ep0=$(date_ep); txt0=$(date_txt); host0=$(date -u +%s)
echo "  A. date ${txt0:-（没有）}  ep=${ep0:-?}  宿主=$host0  差=$(( ${ep0:-0} - host0 ))s"
[ -n "$ep0" ] || { echo "  FAIL  没读到 date 的 epoch"; fail=1; }
d=$(abs $((ep0 - host0)))
[ "$d" -le 180 ] || { echo "  FAIL  开机锚点与宿主机相差 ${d}s（应 ≤180s）"; fail=1; }

# ---- B. 等 12 秒：走时应当涨，且不快于真时间 ----
sleep 12
run "date"
ep1=$(date_ep); host1=$(date -u +%s)
kdelta=$((ep1 - ep0)); hdelta=$((host1 - host0))
echo "  B. 12s 之后：ep 涨了 ${kdelta}s，宿主涨了 ${hdelta}s（tick 不该比真时间快）"
[ "$kdelta" -ge 1 ] || { echo "  FAIL  走时没动（epoch 一直是 $ep0）"; fail=1; }
[ "$kdelta" -le $((hdelta + 2)) ] \
  || { echo "  FAIL  走时比真时间还快（$kdelta > $hdelta），说明没按 tick 走"; fail=1; }

# ---- C. clock：毫秒粒度 ----
run "clock"
clk=$(rows | grep -oE 'clk ep=[0-9]+ ms=[0-9]+ t=0x[0-9A-F]+ d=[+-][0-9]+ s=[0-9]+ r=[0-9]+ x=[0-9]+' | head -1)
ms=$(fld ms); tk=$(rows | grep -oE ' t=0x[0-9A-F]+' | head -1 | sed 's/ t=0x//')
echo "  C. ${clk:-（没有输出）}"
[ -n "$clk" ] || { echo "  FAIL  没读到 clock 行"; fail=1; }
# 0 起头的数字在 bash 里是八进制：ms=010 会被算成 8，%10 判定就假红
# （换到 QEMU 11 + gcc 16 的机器上第一次遇到：那一瞬 ms 恰好落在 010）。
# 用 10# 强制十进制。
[ $(( 10#${ms:-0} % 10 )) = 0 ] \
  || { echo "  FAIL  ms=$ms 不是 10ms 的整数倍（100Hz 下不该有更细的粒度）"; fail=1; }
[ -n "$tk" ] || { echo "  FAIL  clock 行里没有 tick 计数"; fail=1; }

# ---- D. 手动校准一次 ----
s_before=$(fld s)
run "clock sync"
sy=$(rows | grep -oE 'clk sync rc=[0-9E]+ d=[+-][0-9]+ s=[0-9]+ r=[0-9]+' | head -1)
syncrc=$(printf '%s\n' "$sy" | sed -nE 's/.*rc=([0-9E]+).*/\1/p')
sd=$(printf '%s\n' "$sy" | sed -nE 's/.*d=([+-][0-9]+).*/\1/p')
s_after=$(printf '%s\n' "$sy" | sed -nE 's/.*s=([0-9]+).*/\1/p')
r_after=$(printf '%s\n' "$sy" | sed -nE 's/.*r=([0-9]+).*/\1/p')
echo "  D. ${sy:-（没有输出）}"
echo "     账：宿主过了 ${hdelta}s、内核走了 ${kdelta}s，应补约 $((hdelta - kdelta))s"
[ "$syncrc" = "0" ] || { echo "  FAIL  clock sync 没成功（rc=$syncrc）"; fail=1; }
[ -n "${sd:-}" ] || { echo "  FAIL  没读到校准差值 d"; fail=1; }
gap=$(abs $((sd - (hdelta - kdelta))))
echo "     d=$sd  与欠账 $((hdelta - kdelta)) 相差 ${gap}s（允许 ≤10s：两次测量之间的打字/抓屏在 TCG 下会被放大）"
[ "$gap" -le 10 ] || { echo "  FAIL  校准差值与实测漂移对不上"; fail=1; }
[ "${s_after:-0}" -gt "${s_before:-0}" ] || { echo "  FAIL  校准计数 s 没长（s=$s_before -> ${s_after:-?}）"; fail=1; }
[ "${r_after:-1}" = "0" ] || { echo "  FAIL  正常校准不该被拒（r=$r_after）"; fail=1; }

# ---- E/F. 校准后：对得上宿主、年月日自洽 ----
run "date"
ep2=$(date_ep); txt2=$(date_txt); host2=$(date -u +%s)
after=$(abs $((ep2 - host2)))
moved=$(abs $((ep2 - ep1)))
echo "  E. ep=$ep2 与宿主差 ${after}s；相对校准前挪了 ${moved}s（封顶 60s）"
[ "$after" -le 180 ] || { echo "  FAIL  校准后仍与宿主相差 ${after}s"; fail=1; }
[ "$moved" -le 60 ] || { echo "  FAIL  单次校准挪了 ${moved}s，超过封顶"; fail=1; }
want_txt=$(LC_ALL=C date -u -d "@$ep2" '+%Y-%m-%d %H:%M:%S %a')
echo "  F. 内核 ${txt2:-（没有）}  /  GNU date 复核 ep=$ep2：$want_txt"
[ "$txt2" = "$want_txt" ] || { echo "  FAIL  校准后的年月日与它自己给的 epoch 不自洽"; fail=1; }

# ---- G. 把误差设成 20s，校准必须拉回来（确定性证据） ----
run "clock skew 20"
sk=$(rows | grep -oE 'clk skew rc=[0-9E]+ err=[0-9]+' | head -1)
run "date"
ep3=$(date_ep); host3=$(date -u +%s)
behind=$((host3 - ep3))
echo "  G. ${sk:-（没有输出）}；设成 20s 误差后的实测差 = ${behind}s（应约 20s）"
[ "$behind" -ge 15 ] && [ "$behind" -le 27 ] \
  || { echo "  FAIL  skew 之后没有稳定落后 20s（差 ${behind}s）"; fail=1; }
run "clock sync"
sy2=$(rows | grep -oE 'clk sync rc=[0-9E]+ d=[+-][0-9]+ s=[0-9]+ r=[0-9]+' | head -1)
sd2=$(printf '%s\n' "$sy2" | sed -nE 's/.*d=([+-][0-9]+).*/\1/p')
rc2=$(printf '%s\n' "$sy2" | sed -nE 's/.*rc=([0-9E]+).*/\1/p')
echo "     ${sy2:-（没有输出）}"
[ "$rc2" = "0" ] || { echo "  FAIL  20s 的偏差应当能校准（rc=$rc2）"; fail=1; }
# d 不会恰好是 +20：skew 之前内核本来就落后一点，测量窗口里又落后几秒。
# 判"方向与量级对"（≥19 且 ≤32），真正的证据是下面 left < behind。
[ "$sd2" -ge 19 ] && [ "$sd2" -le 32 ] \
  || { echo "  FAIL  校准报的 d=$sd2，应在 19..32 之间（至少要把 20s 补上）"; fail=1; }
run "date"
ep4=$(date_ep); host4=$(date -u +%s)
left=$(abs $((ep4 - host4)))
echo "     拉回后与宿主差 ${left}s（拉回前是 ${behind}s）"
{ [ "$left" -le 5 ] && [ "$left" -lt "$behind" ]; } \
  || { echo "  FAIL  20s 偏差没被拉回来（${behind}s -> ${left}s）"; fail=1; }

# ---- H. 超封顶的偏差必须被拒 ----
run "clock skew 90"
run "clock sync"
sy3=$(rows | grep -oE 'clk sync rc=[0-9E]+ d=[+-][0-9]+ s=[0-9]+ r=[0-9]+' | head -1)
rc3=$(printf '%s\n' "$sy3" | sed -nE 's/.*rc=([0-9E]+).*/\1/p')
r3=$(printf '%s\n' "$sy3" | sed -nE 's/.*r=([0-9]+).*/\1/p')
sd3=$(printf '%s\n' "$sy3" | sed -nE 's/.*d=([+-][0-9]+).*/\1/p')
run "date"
ep5=$(date_ep); host5=$(date -u +%s)
still=$((host5 - ep5))
echo "  H. ${sy3:-（没有输出）}；拒绝后仍差 ${still}s（应仍约 90s）"
[ "$rc3" = "0" ] && { echo "  FAIL  90s 的偏差不该被采纳（封顶 60s）"; fail=1; }
[ "${r3:-0}" -ge 1 ] || { echo "  FAIL  拒绝计数 r 没长（r=${r3:-?}）"; fail=1; }
[ "$still" -ge 60 ] || { echo "  FAIL  被拒的校准却把钟挪回去了（差 ${still}s）"; fail=1; }
run "clock skew 0"
run "clock sync"
sy4=$(rows | grep -oE 'clk sync rc=[0-9E]+ d=[+-][0-9]+ s=[0-9]+ r=[0-9]+' | head -1)
echo "     还原：${sy4:-（没有输出）}"
printf '%s\n' "$sy4" | grep -q 'rc=0' || { echo "  FAIL  还原后校准仍不成功"; fail=1; }
run "date"
ep6=$(date_ep); host6=$(date -u +%s)
final=$(abs $((ep6 - host6)))
echo "     还原后与宿主差 ${final}s"
[ "$final" -le 5 ] || { echo "  FAIL  还原后仍与宿主差 ${final}s"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  两路时间：锚点差 ≤180s、走时由 tick 驱动（12s 涨 ${kdelta}s / 宿主 ${hdelta}s）、"
  echo "      skew20 被拉回（d=$sd2）、skew90 被拒（rc=$rc3, r=$r3）、年月日与 epoch 自洽"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
