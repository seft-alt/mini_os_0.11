/* ==============================================================
 * report：自检界面的“唯一字符串来源”
 *
 * 为什么单独成模块：屏幕只有 24 行、每行最多 79 列，改一个字段名或加一个
 * 数字都要重算列宽。字符串散落在 main() 里时，这件事只能靠人工数格子；
 * 收进这里之后，tools/linewidth.py 可以按“一个 printk 一行”直接算宽度，
 * `make` 会在编译前先把超宽的行打回去。
 *
 * 约定（linewidth.py 依赖它，不要改）：
 *   1. 每一行由**一次 printk 调用**打完，格式串以 '\n' 结尾；
 *   2. 十六进制一律写定宽：%08X（8 位）/ %03X（3 位）/ %02X（2 位）/ %01X（1 位）；
 *   3. 只打印“半行”的函数（标签先打、数字后接）在名字后缀 `_head` / `_tail`，
 *      它们含 '\n' 的分支才计入行宽。
 *
 * 数值口径与原来逐字节一致：print_small_hex 只取低 4 位、print_hex2 只取低 8 位、
 * print_hex3 只取低 12 位、vga_print_hex 固定 8 位。这里的 %01X/%02X/%03X/%
 * 08X 分别对应它们，调用方传“已经取过位的值”即可。
 * ============================================================== */
#ifndef MINI_OS_REPORT_H
#define MINI_OS_REPORT_H

/* row 0：开机横幅 */
void rep_banner(void);

/* row 1：栈现场 + BIOS 给的常规内存（KB） */
void rep_cpu(unsigned long esp, unsigned long ss, unsigned long mem_k);

/* row 2：装好的中断/异常向量表 */
void rep_idt(void);

/* row 3：8254 的频率 / 分频 / 读回计数 */
void rep_pit(unsigned long hz, unsigned long div, unsigned long cnt);

/* row 4：RTC 交叉校验 + 当前 tick/ms（两段频率证据合一行） */
void rep_rtc(unsigned long dsec, unsigned long dtick,
             unsigned long tick, unsigned long ms);
/* row 4 的罕见告警：dtick 超出 40..120 时先打这一行（成功路径不出现） */
void rep_rtc_warn(void);

/* row 5：光标（硬件位置 + 形状）与键盘（PIC 掩码 / 中断次数 / 队列 / Shift） */
void rep_vga_kbd(unsigned long hw, unsigned long shape,
                 unsigned long pic_mask, unsigned long irqs,
                 unsigned long q, unsigned long shift_ok);

/* 排版越界的告警行：**正常路径绝不出现**。
 * 理由：`ovf` 是个只增不减的计数器，在 row 5 打印时只有“前六行”的账，
 * 却被读成“整屏的账”——半截快照当终值用，是这个屏幕最容易自欺的地方。
 * 改成正向检查：整个自检跑完后读一次终值，非 0 就打这一行（row 24 → 上滚），
 * 屏幕上滚必然让 verify 的 24 行基准整体错位，问题立刻暴露，不用谁去盯某个字段。 */
void rep_layout_alarm(unsigned long lines_hit_80);

/* row 6：按需分页窗口 */
void rep_demand(unsigned long base, unsigned long size);

/* row 9：两页读回 + 解除映射后的物理页 */
void rep_read(unsigned long v0, unsigned long v1,
              unsigned long base, unsigned long released);

/* row 11：再读一次（重新缺页）+ 页框水位 */
void rep_touch(unsigned long value, unsigned long used, unsigned long total);

/* row 12：阶段一（时间片）与阶段二（优先级+老化）的 tick 账单 */
void rep_phase12(unsigned long u, unsigned long v, unsigned long s1,
                 unsigned long h, unsigned long l, unsigned long sw);

/* row 13：调用门契约（DPL / 白名单外被拒 / 白名单内旧值）+ 载荷析构 */
void rep_gate(unsigned long dpl, unsigned long deny, unsigned long old,
              unsigned long dtor, unsigned long magic,
              unsigned long seen, unsigned long why);

/* row 14：ring3 + 租约收回三件套 */
void rep_ring3(unsigned long tr, unsigned long cpl, unsigned long ret,
               unsigned long over, unsigned long wild,
               unsigned long set, unsigned long cap,
               const unsigned long knl[4],
               unsigned long forced, unsigned long revoked,
               unsigned long notice);

/* row 15：阶段三的说明行（空入口被拒的证据在同一行尾） */
void rep_phase3_head(unsigned long null_spawn);

/* row 16：C/B 醒来顺序（C 50ms 先于 B 100ms） */
void rep_wake(unsigned long c_slot, unsigned long c_gap,
              unsigned long b_slot, unsigned long b_gap,
              unsigned long seq, unsigned long arg);

/* row 17：D 的槽位 / 收到的参数 / 借用表拒绝次数与水位 */
void rep_d_slot(unsigned long slot, unsigned long arg,
                unsigned long denied, unsigned long peak);

/* row 18：三个 yield 任务各自的 tick + 切换次数 */
void rep_phase4(unsigned long y1, unsigned long y2, unsigned long y3,
                unsigned long sw);

/* row 19：睡眠堆醒来顺序（按 ms 升序排列成 0x3142） */
void rep_phase5(unsigned long order);

/* row 20：历史惩罚 + 睡眠回血（阈值/倍数两段对照） */
void rep_decay(unsigned long start_prio, unsigned long floor_prio,
               unsigned long ticks, unsigned long rest_default,
               unsigned long rest_high, unsigned long rest_back,
               unsigned long deep_long, unsigned long deep_short);

/* row 21：guard 页拦下栈溢出（标签先打，数字由 _tail 接上） */
void rep_guard_head(void);
void rep_guard_tail(unsigned long why);
/* guard 没拦住时任务自己打的半句（失败路径才会出现） */
void rep_guard_not_trapped(void);

/* row 22：收尾账目 + int 0x31 三个返回值 */
void rep_done(unsigned long sw, unsigned long frames_used,
              unsigned long frames_total, unsigned long rq, unsigned long sq,
              unsigned long tune_old, unsigned long tune_bad,
              unsigned long tune_kept);

#endif /* MINI_OS_REPORT_H */
