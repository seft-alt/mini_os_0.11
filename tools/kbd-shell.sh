#!/bin/bash
# shell = 提交行的消费者：验证“行协议 + 内建命令 + 内存输出缓冲”这一层是真的在跑。
#
# 两段：
#   A. 自检 ⑦ 已经把五行固定输入喂给真解释器，读 sh_* 证据：
#        echo hello  -> rc=0，输出 "hello"，长度 5
#        help        -> rc=0，输出缓冲里至少 1 行
#        bogus       -> rc=-1（-1 = 0xFFFFFFFF），未知命令计数 = 1
#        echo "a b"  -> rc=0，argc=2（引号内不切分），输出 "a b"，长度 3
#        echo a b c d e -> rc=-2（0xFFFFFFFE），解析失败计数 = 1
#   B. 端到端：注入 `echo hi` + 回车，看 runs 与 shell_rc 的增量——
#      证明 ISR -> 提交队列 -> kernel_idle -> shell_run 这条链真的连着。
#
# 断言一律用增量（自检自己也压过队列、跑过 shell）。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {                                # rd <符号> -> 十进制
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
rdb() {                               # rdb <符号> <个数> -> 字符（按十六进制字节解码，空格不丢）
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /$2bx 0x$a" | tr -d '\r' \
    | sed -n "s/.*: //p" \
    | python3 -c 'import sys
print("".join(chr(int(t, 16)) for t in sys.stdin.read().split()).rstrip("\x00"))'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}
sext() { case "$1" in 4294967295) echo -1;; 4294967294) echo -2;; *) echo "$1";; esac; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-shell.log 2>&1 &)
sleep 2

want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(eip_now); eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 80))" ]; then
    echo "  已到 kernel_idle（EIP=0x$(printf %08x $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

# ---- A. 自检证据 ----
echo "  A. 自检 ⑦：echo hello"
echo "     rc=$(sext "$(rd sh_rc_echo)") len=$(rd sh_out_echo_len) out='$(rdb sh_out_echo 8)'"
echo "     help: rc=$(sext "$(rd sh_rc_help)") out_lines=$(rd sh_help_lines)"
echo "     bogus: rc=$(sext "$(rd sh_rc_unknown)") unknown=$(rd sh_unknown_n)"
echo "     echo \"a b\": rc=$(sext "$(rd sh_rc_quote)") argc=$(rd sh_quote_argc) len=$(rd sh_out_quote_len) out='$(rdb sh_out_quote 6)'"
echo "     echo a b c d e: rc=$(sext "$(rd sh_rc_argv)") errors=$(rd sh_error_n)"

rc_echo=$(sext "$(rd sh_rc_echo)")
len_echo=$(rd sh_out_echo_len)
buf_echo=$(rdb sh_out_echo 5)
rc_help=$(sext "$(rd sh_rc_help)")
n_help=$(rd sh_help_lines)
rc_unk=$(sext "$(rd sh_rc_unknown)")
n_unk=$(rd sh_unknown_n)
rc_q=$(sext "$(rd sh_rc_quote)")
argc_q=$(rd sh_quote_argc)
len_q=$(rd sh_out_quote_len)
buf_q=$(rdb sh_out_quote 3)
rc_argv=$(sext "$(rd sh_rc_argv)")
n_err=$(rd sh_error_n)

# ---- B. 端到端 ----
runs0=$(rd shell_runs_total); rc0=$(sext "$(rd shell_rc)")
python3 tools/qemu-sendkey.py $QMP e c h o spc h i ret >/tmp/keys.log 2>&1
sleep 1
runs1=$(rd shell_runs_total); rc1=$(sext "$(rd shell_rc)")
cnt=$(rd line_taken_count)
buf_line=$(rdb line_taken_buf 7)
echo "  B. 注入 'echo hi'：runs +$((runs1 - runs0))、shell_rc=$(rd shell_rc)、取走次数=$cnt、行内容='$buf_line'"

fail=0
[ "$rc_echo" = 0 ]   || { echo "  FAIL  echo hello 应返回 0，实际 $rc_echo"; fail=1; }
[ "$len_echo" = 5 ]  || { echo "  FAIL  echo hello 输出长度应为 5，实际 $len_echo"; fail=1; }
[ "$buf_echo" = "hello" ] || { echo "  FAIL  echo hello 输出应为 'hello'，实际 '$buf_echo'"; fail=1; }
[ "$rc_help" = 0 ]   || { echo "  FAIL  help 应返回 0，实际 $rc_help"; fail=1; }
[ "$n_help" -ge 1 ]  || { echo "  FAIL  help 至少应写 1 行输出，实际 $n_help"; fail=1; }
[ "$rc_unk" = -1 ]   || { echo "  FAIL  bogus 应返回 -1，实际 $rc_unk"; fail=1; }
[ "$n_unk" = 1 ]     || { echo "  FAIL  未知命令计数应为 1，实际 $n_unk"; fail=1; }
[ "$rc_q" = 0 ]      || { echo "  FAIL  echo \"a b\" 应返回 0，实际 $rc_q"; fail=1; }
[ "$argc_q" = 2 ]    || { echo "  FAIL  引号内不切分，argc 应为 2，实际 $argc_q"; fail=1; }
[ "$len_q" = 3 ]     || { echo "  FAIL  echo \"a b\" 输出长度应为 3，实际 $len_q"; fail=1; }
[ "$buf_q" = "a b" ] || { echo "  FAIL  echo \"a b\" 输出应为 'a b'，实际 '$buf_q'"; fail=1; }
[ "$rc_argv" = -2 ]  || { echo "  FAIL  六个参数应返回 -2，实际 $rc_argv"; fail=1; }
[ "$n_err" = 1 ]     || { echo "  FAIL  解析失败计数应为 1，实际 $n_err"; fail=1; }
[ "$((runs1 - runs0))" = 1 ] || { echo "  FAIL  注入一行应只让 shell 跑 1 次，实际 $((runs1 - runs0))"; fail=1; }
[ "$rc1" = 0 ]       || { echo "  FAIL  'echo hi' 的返回值应为 0，实际 $rc1"; fail=1; }
[ "$buf_line" = "echo hi" ] || { echo "  FAIL  取走的行应为 'echo hi'，实际 '$buf_line'"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  shell 解释器：行协议/引号/未知命令/超参数/端到端取行 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
