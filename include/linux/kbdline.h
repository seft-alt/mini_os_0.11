/* ==============================================================
 * 第 24 行输入行的内部接口（drivers/kbdline.c 的自有部分）
 *
 * 对外 API 仍在 drivers/char/keyboard.h（`kbd_line_push/take`、`kbd_line_*`、
 * `kbd_echo_*`、`kbd_set_echo_mirror` / `kbd_set_echo_prefix` 都在那里）。
 * 这里只放解码层往输入行递东西的两个入口，以及双方都要认的编辑动作编号：
 *
 *   kbd_echo(c)       —— 一个已解码的字符：可打印就写、'\b' 退格、'\n' 提交
 *   echo_cmd(cmd)     —— 一个已解码的编辑动作：只动光标/删一个字符
 *
 * 为什么动作编号要共享：`keyboard.c` 的扩展码表（0x4B → LEFT 之类）产出它，
 * `kbdline.c` 消费它。编号写在头里，两边不会各说各话。
 * ============================================================== */
#ifndef MINI_OS_KBDLINE_H
#define MINI_OS_KBDLINE_H

/* 扩展码（0xE0 前缀之后那个码）里我们认的几个编辑键 */
#define ECHO_CMD_NONE   0
#define ECHO_CMD_LEFT   1
#define ECHO_CMD_RIGHT  2
#define ECHO_CMD_HOME   3
#define ECHO_CMD_END    4
#define ECHO_CMD_DEL    5
#define ECHO_CMD_UP     6
#define ECHO_CMD_DOWN   7

void kbd_echo(char c);
void echo_cmd(unsigned int cmd);

#endif /* MINI_OS_KBDLINE_H */
