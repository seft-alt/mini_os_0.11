/* ==============================================================
 * report：自检界面的全部打印串（见 include/init/report.h 的约定）
 *
 * 这里不碰任何状态：每个函数只负责“把传进来的数摆到该在的列上”。
 * 数值口径沿用原来的四个手工打印函数：
 *   vga_print_hex → %08X   （固定 8 位）
 *   print_hex3    → %03X   （只取低 12 位）
 *   print_hex2    → %02X   （只取低 8 位）
 *   print_small_hex → %01X （只取低 4 位）
 * 取位在这一侧做（`& 0xF` / `& 0xFF` / `& 0xFFF`），因为“屏幕上一格就是一位”
 * 这件事属于界面，不该让测量那边去截自己的数。
 * ============================================================== */
#include "linux/printk.h"
#include "linux/report.h"
#include "linux/page.h"

void rep_banner(void)
{
    printk("mini-os 0.2  boot OK  boot -> setup -> head -> main\n");
}

void rep_cpu(unsigned long esp, unsigned long ss, unsigned long mem_k)
{
    printk("[cpu] esp=0x%08X ss=0x%08X mem_k=0x%08X\n", esp, ss, mem_k);
}

void rep_idt(void)
{
    printk("[idt] 00 div | 0D gp | 0E pf | 20 timer | 21 kbd | 30 yield | 31 tune\n");
}

void rep_pit(unsigned long hz, unsigned long div, unsigned long cnt)
{
    printk("[pit] 0x%08XHz div=0x%08X cnt=0x%08X\n", hz, div, cnt);
}

void rep_rtc(unsigned long dsec, unsigned long dtick,
             unsigned long tick, unsigned long ms)
{
    printk("[rtc] dsec=0x%08X dtick=0x%08X | tick=0x%08X ms=0x%08X\n",
           dsec, dtick, tick, ms);
}

void rep_rtc_warn(void)
{
    printk("rtc cross-check: unexpected dtick\n");
}

void rep_vga_kbd(unsigned long hw, unsigned long shape,
                 unsigned long pic_mask, unsigned long irqs,
                 unsigned long q, unsigned long shift_ok)
{
    /* hw 是光标格号（最多 2000 = 0x7D0），shape 是 0xEEND 打包的扫描线：
     * 两者都不需要 8 位，缩到 3+4 位就够；irq 从 1 位放宽到 2 位（时钟跑满
     * 一秒就到 100 次，1 位只会一直显示 0x0）。 */
    printk("[vga] hw=0x%03X shape=0x%04X | [kbd] mk=0x%02X irq=0x%02X q=0x%01X sh=0x%01X\n",
           hw & 0xFFFUL, shape & 0xFFFFUL,
           pic_mask & 0xFFUL, irqs & 0xFFUL, q & 0xFUL, shift_ok & 0xFUL);
}

void rep_layout_alarm(unsigned long lines_hit_80)
{
    printk("[vga] OVF ALARM: %02X lines hit 80 cols, layout broken\n",
           lines_hit_80 & 0xFFUL);
}

void rep_demand(unsigned long base, unsigned long size)
{
    printk("[mem] demand base=0x%08X size=0x%08X\n", base, size);
}

void rep_read(unsigned long v0, unsigned long v1,
              unsigned long base, unsigned long released)
{
    printk("[mem] read 0x%08X , 0x%08X | unmap 0x%08X pa=0x%08X\n",
           v0, v1, base, released);
}

void rep_touch(unsigned long value, unsigned long used, unsigned long total)
{
    printk("[mem] touch 0x%08X frames=0x%08X/0x%08X\n", value, used, total);
}

void rep_phase12(unsigned long u, unsigned long v, unsigned long s1,
                 unsigned long h, unsigned long l, unsigned long sw)
{
    printk("[task] p1 U=0x%02X V=0x%02X s1=0x%02X | p2 H=0x%02X L=0x%02X sw=0x%02X\n",
           u & 0xFFUL, v & 0xFFUL, s1 & 0xFFUL,
           h & 0xFFUL, l & 0xFFUL, sw & 0xFFUL);
}

void rep_gate(unsigned long dpl, unsigned long deny, unsigned long old,
              unsigned long dtor, unsigned long magic,
              unsigned long seen, unsigned long why)
{
    printk("[gate] dpl=0x%01X deny=0x%02X old=0x%02X | own dtor=0x%02X mag=0x%02X seen=0x%01X why=0x%01X\n",
           dpl & 0xFUL, deny & 0xFFUL, old & 0xFFUL, dtor & 0xFFUL,
           magic & 0xFFUL, seen & 0xFUL, why & 0xFUL);
}

void rep_ring3(unsigned long tr, unsigned long cpl, unsigned long ret,
               unsigned long over, unsigned long wild,
               unsigned long set, unsigned long cap,
               const unsigned long knl[4],
               unsigned long forced, unsigned long revoked,
               unsigned long notice)
{
    printk("[ring3] tr=0x%02X cpl=0x%01X ret=0x%02X rej=%02X/%02X set=0x%02X cap=0x%03X knl=%01X%01X%01X%01X ls=%01X/%01X/%01X\n",
           tr & 0xFFUL, cpl & 0xFUL, ret & 0xFFUL,
           over & 0xFFUL, wild & 0xFFUL, set & 0xFFUL, cap & 0xFFFUL,
           knl[0] & 0xFUL, knl[1] & 0xFUL, knl[2] & 0xFUL, knl[3] & 0xFUL,
           forced & 0xFUL, revoked & 0xFUL, notice & 0xFUL);
}

void rep_phase3_head(unsigned long null_spawn)
{
    printk("[task] A ret, C 50ms, B 100ms spawn D(0xDEAD0001) null=0x%08X\n",
           null_spawn);
}

void rep_wake(unsigned long c_slot, unsigned long c_gap,
              unsigned long b_slot, unsigned long b_gap,
              unsigned long seq, unsigned long arg)
{
    printk("[task] wake C=0x%01X gc=0x%01X B=0x%01X gb=0x%01X seq=0x%02X arg=0x%08X\n",
           c_slot & 0xFUL, c_gap & 0xFUL, b_slot & 0xFUL, b_gap & 0xFUL,
           seq & 0xFFUL, arg);
}

void rep_d_slot(unsigned long slot, unsigned long arg,
                unsigned long denied, unsigned long peak)
{
    printk("[task] D slot=0x%08X arg=0x%08X lt=0x%01X/0x%01X\n",
           slot, arg, denied & 0xFUL, peak & 0xFUL);
}

void rep_phase4(unsigned long y1, unsigned long y2, unsigned long y3,
                unsigned long sw)
{
    printk("[task] Y x3: Y=0x%08X/0x%08X/0x%08X sw=0x%08X\n", y1, y2, y3, sw);
}

void rep_phase5(unsigned long order)
{
    printk("[task] sq order: 0x%08X\n", order);
}

void rep_decay(unsigned long start_prio, unsigned long floor_prio,
               unsigned long ticks, unsigned long rest_default,
               unsigned long rest_high, unsigned long rest_back,
               unsigned long deep_long, unsigned long deep_short)
{
    printk("[sched] decay prio 0x%01X -> 0x%01X ticks=0x%08X rest=0x%01X/0x%01X/0x%01X deep=0x%01X/0x%01X\n",
           start_prio & 0xFUL, floor_prio & 0xFUL, ticks,
           rest_default & 0xFUL, rest_high & 0xFUL, rest_back & 0xFUL,
           deep_long & 0xFUL, deep_short & 0xFUL);
}

void rep_guard_head(void)
{
    printk("[sched] guard:");
}

void rep_guard_tail(unsigned long why)
{
    printk(" why=0x%01X\n", why & 0xFUL);
}

void rep_guard_not_trapped(void)
{
    printk(" not-trapped");
}

void rep_done(unsigned long sw, unsigned long frames_used,
              unsigned long frames_total, unsigned long rq, unsigned long sq,
              unsigned long tune_old, unsigned long tune_bad,
              unsigned long tune_kept)
{
    printk("[sched] sw=0x%08X frames=0x%08X/0x%08X rq=%01X sq=%01X |tune %02X/%02X/%02X\n",
           sw, frames_used, frames_total, rq & 0xFUL, sq & 0xFUL,
           tune_old & 0xFFUL, tune_bad & 0xFFUL, tune_kept & 0xFFUL);
}
