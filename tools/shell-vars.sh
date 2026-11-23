#!/bin/bash
# shell 持久态：变量表 + 按 owner 分槽 + `$name` 展开。
#
# A 段读自检 ⑧ 的证据（`init/main.c`）：
#   sh_rc_set=0、sh_var_val_len=4、sh_var_val='seft'
#   sh_rc_iso=0、sh_var_iso_len=0        —— 换绑 owner 0 后看不到 owner 1 的变量
#   sh_rc_unset=0、sh_var_after_len=0    —— 回 owner 1 unset 之后就没值了
#   sh_rc_full=-5（0xFFFFFFFB）           —— 6 格塞满后第 7 个 `set` 被拒
# B 段端到端：注入 `set x 42` ⏎ `echo $x` ⏎，用户页应当先回显 `> echo $x`、下一行是 `42`。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
CON=/tmp/vars-console.bin
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
rdb() {
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /$2bx 0x$a" | tr -d '\r' \
    | sed -n "s/.*: //p" \
    | python3 -c 'import sys
print("".join(chr(int(t, 16)) for t in sys.stdin.read().split()).rstrip("\x00"))'
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}
sext() { case "$1" in 4294967291) echo -5;; 4294967295) echo -1;; *) echo "$1";; esac; }

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP $CON; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-vars.log 2>&1 &)
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

rc_set=$(sext "$(rd sh_rc_set)")
vlen=$(rd sh_var_val_len)
vval=$(rdb sh_var_val 4)
rc_iso=$(sext "$(rd sh_rc_iso)")
iso_len=$(rd sh_var_iso_len)
rc_unset=$(sext "$(rd sh_rc_unset)")
after_len=$(rd sh_var_after_len)
rc_full=$(sext "$(rd sh_rc_full)")
echo "  A. set: rc=$rc_set len=$vlen val='$vval' | iso: rc=$rc_iso len=$iso_len"
echo "     unset: rc=$rc_unset 之后 echo \$who 长度=$after_len | 表满 set: rc=$rc_full"

# B 段：真按键 `set x 42` ⏎ `echo $x` ⏎
python3 tools/qemu-sendkey.py $QMP s e t spc x spc 4 2 ret >/tmp/keys-vars1.log 2>&1
sleep 1
python3 tools/qemu-sendkey.py $QMP e c h o spc shift-4 x ret >/tmp/keys-vars2.log 2>&1
sleep 1.2
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $CON" >/dev/null
echo "  --- 用户页 dump ---"
python3 tools/vga-screen.py "$CON" | sed -n '2,10p'
last=$(sext "$(rd shell_rc)")

fail=0
[ "$rc_set" = 0 ]    || { echo "  FAIL  set 应返回 0，实际 $rc_set"; fail=1; }
[ "$vlen" = 4 ]      || { echo "  FAIL  echo \$who 输出长度应为 4，实际 $vlen"; fail=1; }
[ "$vval" = "seft" ] || { echo "  FAIL  echo \$who 输出应为 'seft'，实际 '$vval'"; fail=1; }
[ "$rc_iso" = 0 ]    || { echo "  FAIL  换绑后 echo 应返回 0，实际 $rc_iso"; fail=1; }
[ "$iso_len" = 0 ]   || { echo "  FAIL  owner 0 不该看到 owner 1 的变量（长度应为 0），实际 $iso_len"; fail=1; }
[ "$rc_unset" = 0 ]  || { echo "  FAIL  unset 应返回 0，实际 $rc_unset"; fail=1; }
[ "$after_len" = 0 ] || { echo "  FAIL  unset 之后 echo \$who 应为空，实际长度 $after_len"; fail=1; }
[ "$rc_full" = -5 ]  || { echo "  FAIL  表满时 set 应返回 -5，实际 $rc_full"; fail=1; }
python3 tools/vga-screen.py "$CON" | grep -q "^row .*| > set x 42" \
  || { echo "  FAIL  用户页没有回显 '> set x 42'"; fail=1; }
python3 tools/vga-screen.py "$CON" | grep -q "^row .*| > echo \\\$x" \
  || { echo "  FAIL  用户页没有回显 '> echo \$x'"; fail=1; }
python3 tools/vga-screen.py "$CON" | grep -q "^row .*| 42$" \
  || { echo "  FAIL  用户页没有输出展开后的 '42'"; fail=1; }
[ "$last" = 0 ]      || { echo "  FAIL  'echo \$x' 的返回值应为 0，实际 $last"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  变量表：set/unset/展开/表满拒绝/按 owner 隔离/端到端 全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
