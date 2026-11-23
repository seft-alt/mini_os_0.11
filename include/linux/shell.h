#ifndef MINI_OS_SHELL_H
#define MINI_OS_SHELL_H

/* 最小 shell：把“一行已提交的输入”当命令解释。
 *
 * 通路：IRQ1 -> 第 24 行输入行 -> 回车 -> 提交队列 -> kernel_idle 取走 -> shell_run()。
 * 输出先写 4 行内存滚动缓冲（`shell_out_line()` 可取），再由 `shell_console_open()`
 * 之后的用户页同步显示。
 *
 * 持久态（`struct shell_state`，按 owner 分 4 槽）：
 *   shell_bind(owner)         —— 切绑定（0..3，越界当 0），返回实际槽号；
 *   shell_bound_owner()       —— 当前绑的是谁；
 *   历史（4 条）、变量表（6 格）、计数（runs/unknown/errors/last_rc）都在槽里，
 *   所以两个 owner 的 `hist`/`set` 互不可见。
 *
 * 内建命令：`help` / `echo <...>` / `ver` / `clear` / `hist` / `set [名字 [值...]]` / `unset 名字`；
 * 另有 `!!` 重跑上一条（无历史时 -4）。参数里的 `$name` 展开成变量值（未定义 → 空串）。
 *
 * 返回码：0 成功，-1 未知命令，-2 参数太多（>4），-3 空行，-4 `!!` 无历史，-5 变量表满/名字非法。
 *
 * 用户页（VGA 第 1 页，字节 0xB9000..）：
 *   shell_console_open()     —— 清用户页、写横幅、把 CRTC 切到第 1 页、输入行镜像过去；
 *                               只在自检全打完（main 末尾）调，第 0 页证据一个字节不动。
 *   shell_console_on()       —— 是否已开（0/1）
 *   shell_console_row()      —— 下一条输出写在第几行（0..23）
 *   shell_console_chars()    —— 累计写到用户页的字符数
 *   shell_console_scrolls()  —— 累计上滚次数（满 24 行才滚）
 *
 * 约定：解析与执行都在调用者上下文里同步完成，不睡眠、不分配、不递归。
 */
int shell_run(const char *line, unsigned int len);
unsigned int shell_runs(void);
unsigned int shell_unknown(void);
unsigned int shell_errors(void);
unsigned int shell_last_argc(void);
int shell_last_rc(void);
unsigned int shell_out_count(void);
unsigned int shell_out_len(unsigned int idx);
const char *shell_out_line(unsigned int idx);

/* 命令历史（`hist` 与 `!!` 的证据）：idx 从最新往前数，0 = 最新 */
unsigned int shell_hist_count(void);
const char *shell_hist_line(unsigned int idx);
unsigned int shell_hist_len(unsigned int idx);

/* 变量表（`set` 与 `$name` 的证据）：按槽位 0..5，空槽返回 0 */
unsigned int shell_var_count(void);
const char *shell_var_name(unsigned int idx);
const char *shell_var_value(unsigned int idx);

/* owner 绑定 */
int shell_bind(unsigned int owner);
unsigned int shell_bound_owner(void);

void shell_console_open(void);
unsigned int shell_console_on(void);
unsigned int shell_console_row(void);
unsigned int shell_console_chars(void);
unsigned int shell_console_scrolls(void);

#endif /* MINI_OS_SHELL_H */
