#!/bin/bash
# 租约交接专项：「页归任务之后，时间不再动它的 ttl」。
#
# 背景：exec 在内核名下按 ttl=500 借页，spawn 之后要把这几笔账改记到任务槽位，
# 并改成 PAGE_LEASE_NO_TTL。否则 ring3 放开 IF 之后，用户态跑久了，
# 内核的兜底会在"正跑着的页"上把 U/S 收走。
#
# 断言（前两条在 ring3 采样内完成，第三条在跑完之后）：
#   A. ring3 期间至少一笔租约 ttl=0xFFFFFFFF，且两次以上采样里一分不变，
#      而同一段时间 tasks[1].ticks 在涨（= 时钟真在走，它就是不碰这页）；
#   B. 程序仍按约定结束：exec uprog3 rc=0 mag=0x45584543 val=0xC0DE0003 ex=9；
#   C. 任务死了以后 8 个租约槽全空——页不是"还"回去的，是**跟着槽位一起收**的。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
. build/layout.env 2>/dev/null || { echo "FAIL build/layout.env 不存在，先 make"; exit 1; }
QMP=/tmp/qmp.sock
SCR=/tmp/lsw-screen.bin
IMG=/tmp/lsw.img

killall -q -9 qemu-system-i386 2>/dev/null
sleep 1
rm -f "$QMP" "$SCR"
# 参数顺序：img / uprog / bad / elf / big / paged / huge / uprog3
python3 tools/datadisk.py $IMG build/uprog.bin build/badprog.bin build/uprog.elf \
  '' '' '' build/uprog3.bin >/tmp/lsw-dd.log 2>&1 || { echo "FAIL 造盘失败"; exit 1; }
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:/tmp/lsw-serial.log -qmp unix:$QMP,server,nowait \
   >/tmp/lsw-qemu.log 2>&1 &)
sleep 3
python3 tools/qemu-sendkey.py $QMP e x e c spc u p r o g 3 ret >/dev/null 2>&1

LEASES=$(nm -n build/kernel.elf | awk '$3=="leases"{print "0x"$1; exit}')
TASKS=$(nm -n build/kernel.elf | awk '$3=="tasks"{print "0x"$1; exit}')
[ -n "$LEASES" ] && [ -n "$TASKS" ] || {
    echo "FAIL 找不到 leases / tasks 符号"; killall -q -9 qemu-system-i386; exit 1; }
# task_t 布局来自 build/layout.env（= 编译器对结构体的答案），不再写死 108/+24
TICKS=$(printf '0x%x' $((TASKS + TASK_SIZE + TASK_OFF_TICKS)))

python3 tools/lease-watch.py $QMP "$LEASES" "$TICKS" 300 12 >/tmp/lsw-watch.log 2>&1
head -3 /tmp/lsw-watch.log
V=$(grep -o 'VERDICT .*' /tmp/lsw-watch.log | head -1)
if [ "$V" != "VERDICT ok" ]; then
    echo "FAIL 租约没有真的交给任务：$V"
    python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $SCR" >/dev/null 2>&1
    python3 tools/vga-screen.py $SCR | sed -n '1,8p'
    killall -q -9 qemu-system-i386 2>/dev/null
    exit 1
fi

sleep 4
python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $SCR" >/dev/null 2>&1
SCREEN=$(python3 tools/vga-screen.py $SCR)
python3 tools/qemu-qmp.py $QMP "xp /32dw $LEASES" >/tmp/lsw-after.txt 2>&1

if ! printf "%s\n" "$SCREEN" | grep -q "exec uprog3 rc=0 mag=0x45584543"; then
    echo "FAIL 程序没按约定结束（屏幕没找到 rc=0 mag 自证）"
    printf "%s\n" "$SCREEN" | sed -n '1,8p'
    killall -q -9 qemu-system-i386 2>/dev/null
    exit 1
fi
if ! printf "%s\n" "$SCREEN" | grep -q "val=3235774467 ex=9"; then
    echo "FAIL 退出码/结果值不对（期望 val=0xC0DE0003 / ex=9）"
    printf "%s\n" "$SCREEN" | sed -n '1,8p'
    killall -q -9 qemu-system-i386 2>/dev/null
    exit 1
fi

# C 段：任务死了，租约表必须一笔不剩（8 槽 32 个双字全 0）
LEFT=$(awk -F: '/:/ {n=split($2,a," "); for (i=1; i<=n; i++) if (a[i]+0 != 0) bad++} END{print bad+0}' \
        /tmp/lsw-after.txt)
if [ "$LEFT" -ne 0 ]; then
    echo "FAIL 任务死后租约表还剩 $LEFT 个非零字段（页没跟着槽位收）"
    killall -q -9 qemu-system-i386 2>/dev/null
    exit 1
fi

killall -q -9 qemu-system-i386 2>/dev/null
echo "PASS  租约跟着页一起归任务（ring3 期间 ttl 不动、任务死后 8 槽全空）"
