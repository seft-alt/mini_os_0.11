/* ==============================================================
 * PS/2 键盘（IRQ1，向量 0x21）
 *
 *   kbd_feed(sc)   —— 解码一个扫描码并入队。返回 1 = 真的产出一个字符。
 *                     **中断处理体和自检调用的是同一个函数**，所以自检不是
 *                     “另写一段翻译逻辑”，而是把真路径拿出来单练。
 *   kbd_getchar()  —— 无字符返回 -1（不阻塞）。
 *   kbd_queue_count() / kbd_irq_count() / kbd_dropped() / kbd_pic_mask()
 *                  —— 队列深度、真中断次数、队列满丢弃次数、PIC 屏蔽字读回。
 * ============================================================== */
#ifndef MINI_OS_KEYBOARD_H
#define MINI_OS_KEYBOARD_H

void keyboard_init(void);

int kbd_feed(unsigned char scancode);
int kbd_getchar(void);

unsigned int kbd_queue_count(void);
unsigned int kbd_irq_count(void);
unsigned int kbd_dropped(void);
unsigned int kbd_pic_mask(void);

/* 真按键回显：可打印字符写到屏幕第 24 行（最后一行，自检从不占用）。
 * 没有按键时这一行保持空白，所以它不影响 24 行的屏幕比对；
 * 有按键时它就成了“真中断真的收到字符”的唯一现场。
 * 这一行是可编辑的：退格删光标左边、Del 删光标处、方向键/Home/End 移动、
 * Up/Down 翻 4 条历史、回车 = 提交（入历史 + 进提交队列）并清行。 */
unsigned int kbd_echo_count(void);
unsigned int kbd_echo_erase(void);     /* 退格/Delete 删掉的字符数 */
unsigned int kbd_echo_newline(void);   /* 回车清行的次数 */
unsigned int kbd_echo_wrap(void);      /* 撞满 79 列清行重来的次数 */
unsigned int kbd_echo_move(void);      /* 方向键/Home/End 移动光标的次数 */

/* 回车提交：`line_buf` 的那一份内容从这里交出去（一圈 4 格的短队列，FIFO）。
 *   kbd_line_take(dst, cap) —— 从队头取一条，返回原始长度（0 = 队列空），取走即出队；
 *                             dst/cap 为 0 等价于“丢弃”。不会动到后面排着的几条。
 *   kbd_line_push(s, len)   —— 外部生产者也可入队（回车走的是同一处）；
 *                             返回 1 = 入队，0 = 队满被丢。
 *   kbd_line_pending()      —— 队列里还有几条没被取走（0..容量）。
 *   kbd_line_capacity()     —— 当前容量（默认 4，可配）。
 *   kbd_line_submit_count() —— 累计成功提交次数（空行回车不算）。
 *   kbd_line_drop()         —— 队满导致新提交被丢掉的次数（旧行永远不会被覆盖）。
 * 约定：谁取走谁负责那 79 字节；排队的量级靠“消费者自己流控”，不做无限队列。 */
int kbd_line_take(char *dst, unsigned int cap);
int kbd_line_push(const char *s, unsigned int len);
unsigned int kbd_line_pending(void);
unsigned int kbd_line_capacity(void);
unsigned int kbd_line_submit_count(void);
unsigned int kbd_line_drop(void);

/* 输入行镜像到第二页：`base` 是那一页的字节基址（0 = 关掉镜像）。
 * 第 0 页永远是“证据页”，镜像只是让用户页也看得见自己在打什么。 */
void kbd_set_echo_mirror(unsigned long base);
unsigned long kbd_echo_mirror(void);

/* 提示符前缀：只在镜像页画（第 0 页保持纯输入内容，老断言不受影响）。
 * len 上限 8；硬件光标自动右移 prefix_len。 */
void kbd_set_echo_prefix(const char *s, unsigned int len);
unsigned int kbd_echo_prefix_len(void);

/* 把硬件光标搬回输入行（第 24 行）的当前打字位置，不改输入内容。
 * 为什么需要它：输出区打印时会让光标一路跟着文字下移，一条命令打完后必须
 * 把光标交还给输入行，否则用户下一句不知道该在哪儿打字（提示符旁边没有光标）。 */
void kbd_echo_cursor_to_line(void);

/* 队列归属：每 owner 一条独立环（4 条 × 4 格 = 16 格），入队时按当时归属选环。
 *   kbd_owner_attach()      —— 领一个槽，返回 1..3；0 = 槽满/失败（0 是共享槽）。
 *   kbd_owner_detach(tag)   —— 还槽，并清掉它那条环里的字符。
 *   kbd_owner_select(tag)   —— 之后的按键进谁那条环（0 = 共享）；没 attach 的槽返回 -1。
 *   kbd_owner_active()      —— 当前归属。
 *   kbd_getchar_t(tag)      —— 只从 tag 那条环取一个，空了就 -1（绝不顺手拿别人的）。
 *   kbd_getchar()           —— 等价于 kbd_getchar_t(0)，老调用者行为不变。
 *   kbd_queue_count_t(tag)  —— tag 那条环里还有几个。
 *   kbd_queue_total()       —— 四条环总共还有几个（“队列还干净没”看这个）。
 *   kbd_lane_drop(tag)      —— tag 那条环因满丢掉的字符数。
 *   kbd_lane_peak(tag)      —— tag 那条环的历史最高水位（0..4）。
 * 一条环只能装它自己人的字符：所以一个 owner 打满只会丢它自己的，
 * 不会把别人的字符挤掉（旧“一条大环 + 标签”方案做不到这一点）。 */
int kbd_owner_attach(void);
void kbd_owner_detach(int tag);
int kbd_owner_select(int tag);
int kbd_owner_active(void);
int kbd_getchar_t(int tag);
unsigned int kbd_queue_count_t(int tag);
unsigned int kbd_queue_total(void);
unsigned int kbd_lane_drop(int tag);
unsigned int kbd_lane_peak(int tag);

/* 深度可配（运行时可改，不是编译期定死）：
 *   kbd_lane_depth(tag)          —— 这条环现在用几格（默认 4）。
 *   kbd_lane_set_depth(tag, n)   —— 改成 n 格，clamp 1..8，返回实际值。
 *                                 调小不动已经排着的字符；若缩到装不下当前内容，
 *                                 读写位置对齐（等于从空开始），调用者应先把环取空。
 *   kbd_line_set_capacity(n)     —— 提交队列深度，clamp 1..8，返回实际值（默认 4）。
 *                                 缩小不会让排着的行丢失，只是“再进来就判满”。 */
unsigned int kbd_lane_depth(int tag);
unsigned int kbd_lane_set_depth(int tag, unsigned int depth);
unsigned int kbd_line_set_capacity(unsigned int depth);

/* 调度器接线：任务换人时把归属搬到新任务的标签上（不校验 attach，
 * 标签是任务创建时领到的）。换人但归属没变时不记账。
 *   kbd_owner_switches() —— 归属真的变了几次
 *   kbd_owner_seen()     —— bit n = 槽 n 真的当过按键主人
 *   kbd_owner_last()     —— 最后一次生效的归属
 * 这样“键盘归谁”就不再是内核替所有人做的决定，而是**随调度器走**。 */
void kbd_owner_force(int tag);
unsigned int kbd_owner_switches(void);
unsigned int kbd_owner_seen(void);
unsigned int kbd_owner_last(void);

#endif /* MINI_OS_KEYBOARD_H */
