#!/bin/bash
# 日历时间：年月日 <-> 纪元秒的换算，以及 RTC 读出来的"现在"。
#
# 分四段判：
#   A. `date chk` —— 纯算术自检（闰年、月末、两个锚点、往返、2038 边界、
#      非法字段、格式化补零）。不依赖任何硬件，任何机器上 n 必须等于 ok。
#   B. `date <纪元秒>` —— 三个固定时刻，逐条与宿主机 GNU date **比对**，
#      不是比对写死的字符串：写死的期望值错了，测试也会跟着错。
#   C. 越界（2038-01-19 03:14:08）必须明确拒绝，不能回绕成 1901 年。
#   D. `date` —— 实时读 RTC：与宿主机 UTC 时钟比（±180s，QEMU 起机本身要几秒），
#      再用 GNU date 验证内核算出来的年月日与它自己给的 epoch 自洽。
#
# 为什么值得单独一条：时间换算是"看起来都对"的典型——错一天、错一小时、
# 闰年少一天，肉眼很难发现；而 2038 边界如果回绕，屏幕上会打出一个 1901 年的
# 日期，看起来也"只是个日期"。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/date-console.bin
SER=/tmp/date-serial.log
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
eip_now() { python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'; }
rows() { python3 tools/vga-screen.py "$CON" | sed -n 's/^row *[0-9]*| //p'; }
dump() { python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null; }
# 打一段文本：空格换成 QEMU 的 `spc`，其余字符原样当键名
type_text() {
  local s="$1" keys=() c
  for ((i = 0; i < ${#s}; i++)); do
    c="${s:i:1}"
    [ "$c" = " " ] && c=spc
    keys+=("$c")
  done
  python3 tools/qemu-sendkey.py $QMP "${keys[@]}" ret >/dev/null 2>&1
  sleep 1.0
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $SER; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -serial file:$SER \
   -qmp unix:$QMP,server,nowait >/tmp/q-date.log 2>&1 &)
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

fail=0

# ---- A. 纯算术自检 ----
type_text "clear"; type_text "date chk"; dump
chk=$(rows | grep -oE 'date chk n=[0-9]+ ok=[0-9]+' | head -1)
echo "  A. ${chk:-（没有输出）}"
n=$(printf '%s\n' "$chk" | sed -n 's/.*n=\([0-9]*\).*/\1/p')
k=$(printf '%s\n' "$chk" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')
if [ -z "$n" ] || [ -z "$k" ]; then
  echo "  FAIL  没读到 date chk 的 n/ok"; fail=1
elif [ "$n" != "$k" ]; then
  echo "  FAIL  自检有失败：n=$n ok=$k"; fail=1
elif [ "$n" -lt 10 ]; then
  echo "  FAIL  自检条数太少（n=$n），可能覆盖被改掉了"; fail=1
fi

# ---- B. 三个固定时刻，与 GNU date 对账（年月日时分秒、星期、年内第几天） ----
for ep in 0 1709210096 2147483647; do
  type_text "clear"; type_text "date $ep"; dump
  line=$(rows | grep -oE "utc [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} [A-Za-z]{3} yd=[0-9]{3} ep=$ep" | head -1)
  got=$(printf '%s\n' "$line" | sed -E 's/^utc //; s/ yd=.*//')
  gotyd=$(printf '%s\n' "$line" | sed -n 's/.*yd=\([0-9]\{3\}\).*/\1/p')
  want=$(LC_ALL=C date -u -d "@$ep" '+%Y-%m-%d %H:%M:%S %a')
  wantyd=$(printf '%03d' $((10#$(date -u -d "@$ep" '+%j') - 1)))
  echo "  B. ep=$ep"
  echo "     内核：${got:-（没有）}  yd=${gotyd:-?}"
  echo "     宿主：$want  yd=$wantyd"
  [ "$got" = "$want" ] || { echo "  FAIL  ep=$ep 的日期/星期与宿主机不一致"; fail=1; }
  [ "$gotyd" = "$wantyd" ] || { echo "  FAIL  ep=$ep 的 yday 与宿主机不一致（0 起 vs %j 的 1 起）"; fail=1; }
done

# ---- C. 越界：必须明确拒绝 ----
type_text "clear"; type_text "date 2147483648"; dump
bad=$(rows | grep -x 'date bad epoch (0..2147483647)' | head -1)
echo "  C. 越界：${bad:-（没有）}"
[ -n "$bad" ] || { echo "  FAIL  越界纪元秒没有明确拒绝（回绕了？）"; fail=1; }

# ---- D. 实时：RTC 与宿主机时钟 + 自洽 ----
type_text "clear"; type_text "date"; dump
live=$(rows | grep -oE 'date [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} [A-Za-z]{3} yd=[0-9]{3} ep=[0-9]+' | head -1)
echo "  D. ${live:-（没有输出）}"
ep=$(printf '%s\n' "$live" | sed -n 's/.*ep=\([0-9]*\).*/\1/p')
delta=0
if [ -z "$ep" ]; then
  echo "  FAIL  没读到 RTC 的 epoch"; fail=1
else
  host=$(date -u +%s)
  delta=$((ep - host)); [ "$delta" -lt 0 ] && delta=$((-delta))
  echo "   D2. 与宿主机 UTC 相差 ${delta}s（允许 ±180s）"
  [ "$delta" -le 180 ] || { echo "  FAIL  RTC 时间与宿主机差太多"; fail=1; }
  got=$(printf '%s\n' "$live" | sed -E 's/^date //; s/ yd=.*//')
  want=$(LC_ALL=C date -u -d "@$ep" '+%Y-%m-%d %H:%M:%S %a')
  echo "   D3. 内核换算：$got  用 GNU date 复核同一 epoch：$want"
  [ "$got" = "$want" ] \
    || { echo "  FAIL  实时读数的年月日与它自己给的 epoch 不自洽"; fail=1; }
fi

if [ "$fail" = 0 ]; then
  echo "PASS  日历时间：自检 n=$n 全过、三个固定时刻与 GNU date 一致、"
  echo "      2038 越界被拒、RTC 读数与宿主机相差 ${delta}s 且自洽"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
