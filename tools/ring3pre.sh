#!/bin/bash
# ring3 可抢占专项：`uprog3` 在 ring3 忙等（大约一两秒），期间时钟必须能进来。
#
# 断言方式：一边采样 CS，一边读「槽位 1 的 CPU tick 计数」（tasks[1].ticks）。
#   - 至少要有一刻 CS=0x23（真的在 ring3）；
#   - 在这些 ring3 采样里 ticks 必须涨（≥10 个 tick，100Hz 下约 0.1s）——
#     它涨说明 IRQ0 在用户态被放行、并且确实进了内核把它记在槽位上；
#   - 跑完 exec 仍是 rc=0 / magic 自证 / val=0xC0DE0003 / exit 9。
set -u
cd "${MINI_OS_DIR:-$HOME/mini-os}" || exit 1
. build/layout.env 2>/dev/null || { echo "FAIL build/layout.env 不存在，先 make"; exit 1; }
QMP=/tmp/qmp.sock
SCR=/tmp/r3p-screen.bin
IMG=/tmp/r3p.img

killall -q -9 qemu-system-i386 2>/dev/null
sleep 1
rm -f "$QMP" "$SCR"
# 参数顺序：img / uprog / bad / elf / big / paged / huge / uprog3
# （"uprog3" 这份慢程序在**第 8 个**位置，错位就会变成"exec 一个不存在的名字"）
python3 tools/datadisk.py $IMG build/uprog.bin build/badprog.bin build/uprog.elf \
  '' '' '' build/uprog3.bin >/tmp/r3p-dd.log 2>&1 || { echo "FAIL 造盘失败"; exit 1; }
(qemu-system-i386 -fda build/mini-os.img -boot a -m 16 -display none \
   -drive if=ide,index=1,format=raw,file=$IMG \
   -serial file:/tmp/r3p-serial.log -qmp unix:$QMP,server,nowait \
   >/tmp/r3p-qemu.log 2>&1 &)
sleep 3
python3 tools/qemu-sendkey.py $QMP e x e c spc u p r o g 3 ret >/dev/null 2>&1

TASKS=$(nm -n build/kernel.elf | awk '$3=="tasks"{print "0x"$1; exit}')
[ -n "$TASKS" ] || { echo "FAIL 找不到 tasks 符号"; exit 1; }
# task_t 布局来自 build/layout.env（= 编译器对结构体的答案），不再写死 108/+24
TICKS=$(printf '0x%x' $((TASKS + TASK_SIZE + TASK_OFF_TICKS)))

python3 tools/qmp-watch.py $QMP "$TICKS" 400 12 15 >/tmp/r3p-watch.log 2>&1
sleep 4

python3 tools/qemu-qmp.py $QMP "pmemsave 0xb9000 4000 $SCR" >/dev/null 2>&1
SCREEN=$(python3 tools/vga-screen.py $SCR)

RING=$(awk '$1=="S" && $2=="0023"{print $3}' /tmp/r3p-watch.log)
N_RING=$(printf "%s\n" $RING | grep -c . || true)
if [ "$N_RING" -eq 0 ]; then
    echo "FAIL 采样里一次都没抓到 CS=0x23（ring3 没被观察到）"
    tail -3 /tmp/r3p-watch.log
    printf "%s\n" "$SCREEN" | sed -n '1,8p'
    killall -q -9 qemu-system-i386 2>/dev/null
    exit 1
fi

FIRST=$(printf "%s\n" $RING | head -1)
LAST=$(printf "%s\n" $RING | tail -1)
DELTA=$((LAST - FIRST))
echo "  ring3 采样 $N_RING 次，槽位 1 的 tick: $FIRST -> $LAST（涨 $DELTA）"
if [ "$DELTA" -lt 10 ]; then
    echo "FAIL ring3 期间 tick 只涨了 $DELTA，时钟没有真的进来（IF 还是 0？）"
    killall -q -9 qemu-system-i386 2>/dev/null
    exit 1
fi

if ! printf "%s\n" "$SCREEN" | grep -q "exec uprog3 rc=0 mag=0x45584543"; then
    echo "FAIL exec uprog3 没有正常收尾（期望 rc=0 / magic 自证）"
    printf "%s\n" "$SCREEN" | sed -n '1,8p'
    killall -q -9 qemu-system-i386 2>/dev/null
    exit 1
fi
if ! printf "%s\n" "$SCREEN" | grep -q "val=3235774467 ex=9"; then
    echo "FAIL 结果值/退出码不对（期望 val=0xC0DE0003=3235774467、ex=9）"
    killall -q -9 qemu-system-i386 2>/dev/null
    exit 1
fi

echo "PASS  ring3 可被时钟打断：忙等期间槽位 1 的 tick 涨了 $DELTA，程序仍按约定退出"
killall -q -9 qemu-system-i386 2>/dev/null

