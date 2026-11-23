/* ==============================================================
 * shell 的状态层：每个 owner 一份的持久态 + 参数缓冲 + 证据读取
 *
 * 为什么单独成一层：命令解释、变量表、用户页输出是三件不同的事，
 * 但它们都要摸同一份“当前 owner 的状态”。之前这些静态变量全躺在
 * shell.c 里，任何一处新逻辑都能顺手读到 `states[bound]` 的任意字段，
 * 文件也就长到了 700 行。收进这里之后，另外三个文件只拿 `ST` 指针，
 * 想改哪个字段就得先在这里找到它——边界是结构而不是自觉。
 * ============================================================== */
#ifndef MINI_OS_SHELL_STATE_H
#define MINI_OS_SHELL_STATE_H

#define SHELL_ARGC_MAX  4
#define SHELL_ARG_MAX   24

/* 命令的返回码：分派层与每个 `cmd_xxx` 共用（命令实现拆到别的文件之后，
 * 这些值必须挂在共享头上，否则每个文件各自写一份就成了“同一个 0 有好几个名字”）。 */
#define SHELL_RC_OK        0
#define SHELL_RC_UNKNOWN  (-1)
#define SHELL_RC_ARGV     (-2)
#define SHELL_RC_EMPTY    (-3)
#define SHELL_RC_NOHIST   (-4)
#define SHELL_RC_VAR      (-5)
#define SHELL_RC_LONG     (-6)            /* 单个参数超过 SHELL_ARG_MAX：整行拒掉 */
/* 输出缓冲的行数：一条命令的输出先落这里，再一次性搬到用户页。
 * 之所以从 4 提到 8：`tasks` 一条命令就是 5 行（slot 0 已跑完/槽位 1..4），
 * 缓冲只有 4 行时最旧的 slot 0 那行会被自己的输出挤掉——"命令打不下自己的结果"
 * 比"缓冲占几百字节"糟得多。8 行 × 79 字节 = 632 字节，代价可以接受。 */
#define SHELL_OUT_LINES 8
#define SHELL_OUT_COLS  79

#define SHELL_HIST      4
#define SHELL_STATES    4
#define SHELL_VARS      6
#define SHELL_NAME_MAX  8
#define SHELL_VAL_MAX   24

/* ---- 每个 owner 一份的持久态 ---- */
struct shell_state {
    char hist[SHELL_HIST][SHELL_OUT_COLS];
    unsigned int hist_len[SHELL_HIST];
    unsigned int hist_n;                  /* 0..SHELL_HIST */
    unsigned int hist_head;               /* 下一条写哪 */

    char var_name[SHELL_VARS][SHELL_NAME_MAX];
    char var_val[SHELL_VARS][SHELL_VAL_MAX];
    unsigned int var_used[SHELL_VARS];

    unsigned int runs;
    unsigned int unknown_count;
    unsigned int error_count;
    int last_rc;
};

/* 当前绑定的 owner 那一份（`shell_bind` 决定是哪一份） */
struct shell_state *shell_state(void);
#define ST (shell_state())

/* 参数缓冲区：解析器和命令共用，所以只能是全局；只有 shell/ 里的人碰它 */
extern char shell_argv_buf[SHELL_ARGC_MAX][SHELL_ARG_MAX];
extern unsigned int shell_argv_len[SHELL_ARGC_MAX];
extern unsigned int shell_argc;

/* 跨 owner 的累计次数：只给测试用 `xp` 读，不进任何 state */
extern unsigned int shell_runs_total;

/* 没有 libc，两个够用的字符串函数（放在状态层是为了别处都能用同一份语义） */
int shell_str_eq(const char *a, const char *b, unsigned int n);
int shell_str_len(const char *s, unsigned int max);

/* ---- 证据读取（屏幕排满，只能从内存看） ---- */
unsigned int shell_runs(void);
unsigned int shell_unknown(void);
unsigned int shell_errors(void);
unsigned int shell_last_argc(void);
int          shell_last_rc(void);

unsigned int shell_hist_count(void);
const char  *shell_hist_line(unsigned int idx);
unsigned int shell_hist_len(unsigned int idx);

unsigned int shell_var_count(void);
const char  *shell_var_name(unsigned int idx);
const char  *shell_var_value(unsigned int idx);

#endif /* MINI_OS_SHELL_STATE_H */
