/* ==============================================================
 * 键盘分环队列的内部接口（drivers/kbd_queue.c 的自有部分）
 *
 * 对外 API 仍在 drivers/char/keyboard.h（`kbd_getchar_t` / `kbd_owner_*` /
 * `kbd_lane_*` 都已在那里声明）。这里只放**跨文件必需**的三样：
 *   - 槽数 / 每条环的最大格数 / 默认格数（`kbdline.c`、`keyboard.c` 不直接用它，
 *     但 `kbd_queue.c` 之外的任何地方若要按槽遍历都从这里取，避免写死 4）；
 *   - `kbd_queue_push()` —— 解码层把“已解出的字符”交给队列的唯一入口。
 * ============================================================== */
#ifndef MINI_OS_KBD_QUEUE_H
#define MINI_OS_KBD_QUEUE_H

#define KBD_OWNERS     4             /* 0 = 共享/内核，1..3 = attach 出来的槽 */
#define KBD_LANE_MAX   8             /* 每条环最多几格（BSS：4 条 × 8 字节） */
#define KBD_LANE_DEF   4             /* 默认几格：够挡“打字比消费者快”的抖动 */

/* 把字符放进“当前归属”那条环。
 * 返回 1 = 进队；0 = 这条环满（`drop_count` 与 `lane_drop[]` 各记一笔）。
 * 满只丢自己这条环的字符，别的一条不受影响——这就是 per-owner 隔离的全部含义。 */
int kbd_queue_push(char c);

#endif /* MINI_OS_KBD_QUEUE_H */
