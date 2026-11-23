/* ==============================================================
 * shell 的输出层：8 行内存滚动缓冲 + VGA 第 1 页（用户页）控制台
 *
 * 分出来的是“一条命令的结果怎么落到屏幕/缓冲”这件事：它是唯一直接写
 * `0xB9000` 显存的地方，也是唯一维护 con_row/con_col/con_chars 的地方。
 * 解释器只调用 `shell_out_puts` / `shell_con_report`，不再碰显存，
 * “谁写了第 0 页”这种事故也就从结构上排掉了。
 * ============================================================== */
#ifndef MINI_OS_SHELL_CONSOLE_H
#define MINI_OS_SHELL_CONSOLE_H

/* 用户页是 24 行、每行 79 列：第 24 行留给键盘输入行 */
#define SHELL_CON_ROWS 24
#define SHELL_CON_COLS 79

/* 开用户页：清第 1 页、写横幅、切 CRTC、把输入行镜像过去 */
void shell_con_open(void);

/* 直接往用户页写（只用来打提示符/回显/告警这类不给缓冲的短句） */
void shell_con_puts(const char *s, unsigned int n);
void shell_con_newline(void);

/* 执行前的快照，和“把这一行新产生的输出搬到用户页” */
void shell_con_mark(unsigned int *head, unsigned int *total);
void shell_con_report(unsigned int head0, unsigned int total0);

/* `clear`：把显存与缓冲都抹掉（下一行输出前生效，避免与回显抢顺序） */
void shell_con_clear_next(void);

/* 8 行内存滚动缓冲（满了覆盖最旧那条——这是“滚动”，不是丢行） */
void shell_out_puts(const char *s, unsigned int n);
void shell_out_reset(void);

/* 证据接口 */
unsigned int shell_console_on(void);
unsigned int shell_console_row(void);
unsigned int shell_console_col(void);
/* 最近一次写进 CRTC 的字符格（用户页内偏移）：输出时光标跟着文字走，
 * 所以它应当等于 row*80+col，而不是停在输入行的 24*80 上。 */
unsigned int shell_console_cursor(void);
unsigned int shell_console_chars(void);
unsigned int shell_console_scrolls(void);
unsigned int shell_out_count(void);
unsigned int shell_out_len(unsigned int idx);
const char  *shell_out_line(unsigned int idx);

#endif /* MINI_OS_SHELL_CONSOLE_H */
