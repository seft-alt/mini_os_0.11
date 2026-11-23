#!/bin/bash
# 双页控制台：验证“shell 有看得见的出口”这件事真的成立。
#
# 设计（见 README「用户页」一节）：
#   第 0 页 0xB8000 = 证据页，verify / pmemsave 的基准，永不改动；
#   第 1 页 0xB9000 = 用户页，横幅 + 每条命令的回显与输出，CRTC 起始地址切过去；
#   输入行 row 24 两边都画（键盘 `kbd_set_echo_mirror`），所以切页之后照样看得见打字。
#
# 断言：
#   1. 开页证据：console_start=0x800（0x1000 字节 / 2）、console_chars=37（横幅长度）；
#   2. 用户页 dump 里有横幅 "mini-os shell 0.1"；
#   3. 注入 `help` + 回车后用户页出现 "> help" 和命令表；再注入 `stat` 后出现计数器行；
#   4. 第 0 页原封不动：仍然能看到 row 1 的 esp= 那行（证明没被用户输出踩掉）。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/console.bin
P0=/tmp/console-p0.bin
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON $P0; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-console.log 2>&1 &)
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

start=$(rd console_start); chars0=$(rd con_chars)
echo "  console_start=0x$(printf %x "$start")（期望 0x800）con_chars=$chars0"

python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb8000 4000 $P0"  >/dev/null
echo "  --- 用户页 dump ---"
python3 tools/vga-screen.py "$CON" | sed -n '2,6p'

python3 tools/qemu-sendkey.py $QMP h e l p ret >/tmp/keys-console.log 2>&1
sleep 1.2
# `stat`：把 kernel/stat.h 的计数器摆成一行（可观测性在用户页上的落点）
python3 tools/qemu-sendkey.py $QMP s t a t ret >/tmp/keys-stat.log 2>&1
sleep 1.0
chars1=$(rd con_chars); row=$(rd con_row); scr=$(rd con_scrolls)
ccell=$(rd con_cursor_cell); ccol=$(rd con_col)
echo "  注入 'help'：con_chars $chars0 -> $chars1、con_row=$row、con_scrolls=$scr"
echo "  光标：con_cursor_cell=$ccell  con_row=$row con_col=$ccol（期望 $((row * 80 + ccol))，即输出末尾而不是输入行）"

python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb8000 4000 $P0"  >/dev/null
echo "  --- 用户页 dump（注入后） ---"
python3 tools/vga-screen.py "$CON" | sed -n '2,8p'

# 输入行前缀：只打 `hi` 不回车，用户页 row 24 应当是 "> hi"，证据页仍是 "hi"
python3 tools/qemu-sendkey.py $QMP h i >/tmp/keys-console2.log 2>&1
sleep 0.8
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 /tmp/console-typing.bin" >/dev/null
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb8000 4000 /tmp/console-typing-p0.bin" >/dev/null
echo "  打字未回车："
python3 tools/vga-screen.py /tmp/console-typing.bin | grep '^row 24' || echo "  (用户页 row 24 空)"
python3 tools/vga-screen.py /tmp/console-typing-p0.bin | grep '^row 24' || echo "  (证据页 row 24 空)"
python3 tools/qemu-sendkey.py $QMP ret >/tmp/keys-console3.log 2>&1
sleep 0.8

# 命令历史：echo a ⏎ 再 !! ⏎ —— 用户页上应当出现两次 "> echo a"（第二次是 !! 展开的）
python3 tools/qemu-sendkey.py $QMP e c h o spc a ret >/tmp/keys-console4.log 2>&1
sleep 0.8
python3 tools/qemu-sendkey.py $QMP shift-1 shift-1 ret >/tmp/keys-console5.log 2>&1
sleep 1.0
last=$(rd shell_rc)
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null
echo "  --- 用户页 dump（hist / !!） ---"
python3 tools/vga-screen.py "$CON" | sed -n '2,12p'
echo "  shell_rc=$last"

fail=0
[ "$(printf '%x' "$start")" = "800" ] || { echo "  FAIL  console_start 应为 0x800，实际 0x$(printf %x "$start")"; fail=1; }
# 横幅长度不再写死：命令表每加一条横幅就变一次（加 irq 就从 60 变 64），
# 直接拿 dump 出来的 row 0 文本长度去比 con_chars，两边同源才不会互相漂。
row0_len=$(python3 tools/vga-screen.py "$CON" | sed -n 's/^row  0| //p' | head -1 | awk '{print length($0)}')
[ "$chars0" = "$row0_len" ] || { echo "  FAIL  横幅字符数 con_chars=$chars0 与 row 0 实际长度 $row0_len 不一致"; fail=1; }
python3 tools/vga-screen.py "$CON" | grep -q "^row  0| mini-os shell 0.2" \
  || { echo "  FAIL  用户页 row 0 没有横幅"; fail=1; }
python3 tools/vga-screen.py "$CON" | grep -q "^row  1| > help" \
  || { echo "  FAIL  用户页 row 1 没有回显 '> help'"; fail=1; }
python3 tools/vga-screen.py "$CON" | grep -q "^row  2| help echo ver clear hist set unset stat" \
  || { echo "  FAIL  用户页 row 2 没有命令表"; fail=1; }
python3 tools/vga-screen.py "$CON" | grep -qF "stat pf=0x" \
  || { echo "  FAIL  用户页缺少 stat 计数器行（kernel/stat 的计数没上屏）"; fail=1; }
# `ovf` 必须是 000：自检 24 行全在 79 列以内，任何一行写满 80 列都会在行尾
# 的 '\n' 处多顶一行、上滚之后整屏错位。这是这个数唯一的正常取值。
python3 tools/vga-screen.py "$CON" | grep -qF "ovf=0x000" \
  || { echo "  FAIL  排版溢出计数不是 0（有行写到了 80 列）"; fail=1; }
[ "$chars1" -gt "$chars0" ] || { echo "  FAIL  写用户页后字符数应增加，实际 $chars0 -> $chars1"; fail=1; }
python3 tools/vga-screen.py /tmp/console-typing.bin | grep -q "^row 24| > hi" \
  || { echo "  FAIL  用户页输入行应为 '> hi'（前缀只画镜像页）"; fail=1; }
python3 tools/vga-screen.py /tmp/console-typing-p0.bin | grep -q "^row 24| hi" \
  || { echo "  FAIL  证据页输入行应仍是 'hi'（前缀不许污染证据页）"; fail=1; }
python3 tools/vga-screen.py "$P0" | grep -qF "[cpu] esp=0x" \
  || { echo "  FAIL  第 0 页证据被踩掉了（row 1 没有 esp=）"; fail=1; }
python3 tools/vga-screen.py "$P0" | grep -q "unhandled" \
  || { echo "  FAIL  第 0 页证据被踩掉了（row 23 没有 unhandled 那行）"; fail=1; }
cnt_echo=$(python3 tools/vga-screen.py "$CON" | grep -c "^row .*| > echo a")
[ "$cnt_echo" -ge 2 ] || { echo "  FAIL  '!!' 应把 'echo a' 再跑一遍（用户页应有 >=2 条 '> echo a'），实际 $cnt_echo"; fail=1; }
[ "$last" = "0" ] || { echo "  FAIL  '!!' 的返回值应为 0，实际 $last"; fail=1; }
# 光标跟着输出走：写完 `help`+`stat` 之后，最后写进 CRTC 的格子必须是"输出末尾"
# （row*80+col），而不是停在输入行的 24*80——停在 24*80 就是没跟上换行的老毛病。
[ "$ccell" = "$((row * 80 + ccol))" ] \
  || { echo "  FAIL  光标没跟着输出走：con_cursor_cell=$ccell，期望 $((row * 80 + ccol))"; fail=1; }
[ "$ccell" != "$((24 * 80))" ] \
  || { echo "  FAIL  光标停在输入行（24*80），说明换行时没搬"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  双页控制台：CRTC 切页、横幅、命令回显与输出、证据页零改动"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
