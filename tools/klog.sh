#!/bin/bash
# 内核日志环 + `log` 命令：证明"屏幕上的行滚掉之后还留得住"。
#
# 两段证据：
#   A 段读内存：k_count（环里有几条）、k_dropped（超宽丢了多少字符）、
#              k_overwrote（环满以后覆盖了几条）；
#   B 段看屏幕：敲 `log` ⏎，用户页每行都应当是 "t=0xNNNNNN <原文>"，
#              且能认出几个熟悉的标签（[cpu] / [mem] / unhandled）。
# 只有 A、B 都对，"printk 的字符流 → 环 → shell"这条链才算通：
# A 对 B 错是渲染问题，B 对 A 错是记账问题。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/klog-console.bin
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  [ -n "$a" ] || { echo 0; return; }
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}
screen() { python3 tools/vga-screen.py "$CON"; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-klog.log 2>&1 &)
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

cnt=$(rd k_count); drop=$(rd k_dropped); over=$(rd k_overwrote)
echo "  A. klog: k_count=$cnt  dropped=$drop  overwrote=$over"

# 环里的原文（内存侧）：k_text 是 32 行 × 67 列的定长数组，第 1 格是最早的第 2 行。
# 为什么要读内存：24 行日志在 24 行的用户页上必然滚掉前面几条，
# 屏幕只能证明"渲染对"，"全量还在"得由环本身证明。
kt=$(sym k_text)
# 环里前三条（0 = 最旧）：解码成字符串，用来在"靠前的内容一定滚出屏幕"之后
# 还能证明它们确实进过环。不写死"第几条是 [cpu]"——启动日志的条数会随驱动增加
# （比如加了 [ata] 之后 [cpu] 就从第 2 条挪到了第 3 条），断言按"前三条里有没有"来写。
rows=""
for slot in 0 1 2; do
  off=$(printf '%x' $((0x$kt + slot * 67)))
  txt=$(python3 tools/qemu-qmp.py $QMP "xp /67bx 0x$off" | tr -d '\r' | sed -n 's/.*: //p' \
    | python3 -c 'import sys;print("".join(chr(int(t,16)) for t in sys.stdin.read().split()).replace("\x00",""))')
  rows="$rows$txt
"
  echo "  A2. 环里第 $((slot + 1)) 条=[$txt]"
done

python3 tools/qemu-sendkey.py $QMP l o g ret >/tmp/keys-klog.log 2>&1
sleep 1.2
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null
echo "  --- 用户页 dump（前 3 条 log） ---"
screen | grep -E '^\s*row.*\| t=' | head -3
echo "  --- 用户页 dump（最后 2 行） ---"
screen | tail -3

lines=$(screen | grep -oE 't=0x[0-9A-F]{6} .*' || true)
n_lines=$(printf '%s\n' "$lines" | grep -c . )

fail=0
[ "$cnt" -ge 20 ] 2>/dev/null || { echo "  FAIL  环里只有 $cnt 条，启动日志没有全进环"; fail=1; }
[ "$cnt" -le 32 ] 2>/dev/null || { echo "  FAIL  k_count=$cnt 超出 32（环写坏了）"; fail=1; }
[ "$n_lines" -ge 5 ] || { echo "  FAIL  屏幕上 't=0x……' 的行只有 $n_lines 条"; fail=1; }
printf '%s\n' "$lines" | grep -q "\[cpu\]"        || printf '%s\n' "$rows" | grep -q "\[cpu\]" \
  || { echo "  FAIL  log 里没有 [cpu] 行（环里前三条也没有）"; fail=1; }
printf '%s\n' "$lines" | grep -q "\[mem\]"        || { echo "  FAIL  log 里没有 [mem] 行"; fail=1; }
printf '%s\n' "$lines" | grep -q "unhandled"      || { echo "  FAIL  log 里没有 unhandled 那行"; fail=1; }
screen | grep -q '> log' || { echo "  FAIL  用户页没有回显 '> log'"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  日志环：内存 $cnt 条，屏幕 $n_lines 行带 tick 前缀，启动那 24 行滚掉也还在"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
