#!/bin/bash
# 回车 = 一次提交：验证“提交槽 -> 空闲循环取走”这条链真的通了。
#
# 为什么要单独一个脚本：屏幕 24 行已经排满，提交的现场只能从内存里看——
# 注键之后用 QMP `xp` 读 `line_taken_len` / `line_taken_count` /
# `line_taken_buf`，以及提交槽自己的 `submit_pending` / `submit_count` / `submit_drop`。
#
# 不变量：
#   1. 打字 + 回车之后，空闲循环必须把这一行取走（count=1、len=2、内容 hi）；
#   2. 取走即出队：`submit_pending` 必须回到 0（否则说明“取走”只是复制、没消费）；
#   3. `submit_drop` 保持 0（空闲循环一直在 hlt，不会让提交积压）。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
QMP=/tmp/qmp.sock
sym() { nm -n build/kernel.elf | awk -v s="$1" '$3==s{print $1; exit}'; }
rd() {                                # rd <符号> -> 十进制（xp 会左侧补空格，取最后一列）
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /1dw 0x$a" | tr -d '\r' | awk '{print $NF}'
}
rdb() {                               # rdb <符号> <个数> -> 一串字符（去掉引号与空格）
  local a; a=$(sym "$1")
  python3 tools/qemu-qmp.py $QMP "xp /$2cb 0x$a" | tr -d '\r' \
    | sed -n "s/.*: //p" | tr -d "\n' "
}
eip_now() {
  python3 tools/qemu-qmp.py $QMP 'info registers' | sed -n 's/.*EIP=\([0-9a-f]*\).*/\1/p'
}

killall -q -9 qemu-system-i386 2>/dev/null
rm -f $QMP; sleep 1
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -qmp unix:$QMP,server,nowait >/tmp/q-line.log 2>&1 &)
sleep 2

# 注入前必须等 EIP 落进 kernel_idle：自检里有最长 4s 的 RTC 等待，早注会被吃掉
want=$(sym kernel_idle); want=$((0x$want))
ok=0
for i in $(seq 1 40); do
  eip=$(eip_now); eip=$((0x${eip:-0}))
  if [ "$eip" -ge "$want" ] && [ "$eip" -lt "$((want + 64))" ]; then
    echo "  已到 kernel_idle（EIP=0x$(printf %08x $eip)）"; ok=1; break
  fi
  sleep 0.5
done
[ "$ok" = 1 ] || { echo "  FAIL  没等到 kernel_idle"; killall -q -9 qemu-system-i386; exit 1; }

sc0=$(rd submit_count); sd0=$(rd submit_drop)
python3 tools/qemu-sendkey.py $QMP h i ret >/dev/null
sleep 1

len=$(rd line_taken_len); cnt=$(rd line_taken_count); buf=$(rdb line_taken_buf 2)
sl=$(rd submit_pending); sc=$(rd submit_count); sd=$(rd submit_drop)
sc=$((sc - sc0)); sd=$((sd - sd0))    # 只看这次按键带来的增量：自检也压过队列，绝对值不可靠
echo "  line_taken_len=$len line_taken_count=$cnt line_taken_buf='$buf'"
echo "  submit_pending=$sl 本次 +submit=$sc +drop=$sd"

fail=0
[ "$len" = "2" ]     || { echo "  FAIL  取走的长度应为 2，实际 $len"; fail=1; }
[ "$cnt" = "1" ]     || { echo "  FAIL  取走次数应为 1，实际 $cnt"; fail=1; }
[ "$buf" = "hi" ]    || { echo "  FAIL  取走内容应为 'hi'，实际 '$buf'"; fail=1; }
[ "$sl" = "0" ]      || { echo "  FAIL  取走之后提交队列必须清空（submit_pending=0），实际 $sl"; fail=1; }
[ "$sc" = "1" ]      || { echo "  FAIL  这次回车应只提交 1 条，实际 $sc"; fail=1; }
[ "$sd" = "0" ]      || { echo "  FAIL  正常回车不该被丢（drop 增量为 0），实际 $sd"; fail=1; }

if [ "$fail" = 0 ]; then
  echo "PASS  回车提交 -> 空闲循环取走：内容、长度、消费与计数全部符合预期"
else
  echo "FAIL  上一行是失败原因"
fi
killall -q -9 qemu-system-i386 2>/dev/null
[ "$fail" = 0 ]
