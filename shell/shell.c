/* mini-os 的最小 shell：把“一行已提交的输入”真的当命令解释。
 *
 * 它回答的是键盘那套通路最后缺的一块：按键 -> 输入行 -> 提交队列 -> **谁来解释**。
 * 解释结果落进 4 行内存滚动缓冲（`shell/console.c`），再由用户页（VGA 第 1 页）同步显示。
 *
 * 六个文件的分工（前两版：728 行的 shell.c → 4 个文件；这一版再把 26 个命令
 * 按域拆出去，shell.c 从 1631 行降到 381 行）：
 *   `shell/state.c`   —— 每个 owner 一份的持久态、参数缓冲、证据读取
 *   `shell/var.c`     —— 变量表（set / unset / `$name` 展开的存放处）
 *   `shell/console.c` —— 4 行滚动缓冲 + 用户页显存（唯一写 0xB9000 的地方）
 *   `shell/fmt.c`     —— 十六进制/十进制/有符号/字面量与返回码（所有命令共用）
 *   `shell/cmd_fs.c`  —— disk / fs / ls / cat / fd / write / rm（文件系统与磁盘）
 *   `shell/cmd_prog.c`—— pgd / exec / sys（程序与地址空间）
 *   `shell/cmd_info.c`—— stat / irq / tasks / log / heap / kstress / clock / date / panic
 *   本文件            —— 内建命令（help/hist/echo/ver/clear/set/unset）、解析、
 *                        命令表与分派，以及 `!!` 这种语义上的小把戏
 *
 * 拆分之后本文件只剩“解释”这一件事：它读 ST（状态指针）、调 var_* /
 * shell_out_* / shell_con_*，自己不维护任何静态变量——所以再加命令不会让
 * 状态和输出又长回到这里。命令实现搬出去之后，新增的接口只有两个头文件：
 * `include/shell/cmds.h`（一组 `int cmd_xxx(void)` 声明）与
 * `include/shell/fmt.h`（格式化 helper）。
 *
 * 解析规则（故意小，但都是真的）：
 *   - 空格分隔；连续的空白算一个分隔；
 *   - 以 `"` 开头的参数一直吃到下一个 `"`，中间的空格不切分；
 *   - 参数里的 `$name` 在这时展开（找不到就展开成空串，不报错）；
 *   - 最多 4 个参数（含命令名），超了就是 `SHELL_RC_ARGV`，不猜；
 *   - 参数超过 23 字节截断（和一行 79 字节的整体长度约束一致）。
 */
#include "linux/shell.h"
#include "linux/state.h"
#include "linux/console.h"
#include "linux/var.h"
#include "linux/fmt.h"
#include "linux/cmds.h"
#include "linux/stat.h"

/* 返回码宏在 include/shell/state.h（命令实现散在四个文件，必须共用一份） */

/* ---- 命令历史：跟着 owner 走（`!!` 取最近一条除自己以外的命令）---- */
static void hist_record(const char *line, unsigned int len)
{
    unsigned int i;

    if (len > SHELL_OUT_COLS)
        len = SHELL_OUT_COLS;
    for (i = 0; i < len; i++)
        ST->hist[ST->hist_head][i] = line[i];
    ST->hist_len[ST->hist_head] = len;
    ST->hist_head = (ST->hist_head + 1) % SHELL_HIST;
    if (ST->hist_n < SHELL_HIST)
        ST->hist_n++;
}

/* 把 argv[from..argc-1] 用单空格拼成一行再写出去（`echo` 的输出形态） */
static void out_puts_args(unsigned int from)
{
    char line[SHELL_OUT_COLS];
    unsigned int i, k = 0, j;

    for (i = from; i < shell_argc && k < SHELL_OUT_COLS; i++) {
        if (i > from && k < SHELL_OUT_COLS)
            line[k++] = ' ';
        for (j = 0; j < shell_argv_len[i] && k < SHELL_OUT_COLS; j++)
            line[k++] = shell_argv_buf[i][j];
    }
    shell_out_puts(line, k);
}

/* ---- 内建命令表 ---- */
static int cmd_help(void)
{
    /* 一行装不下 14 条命令（79 列是硬上限），拆两行：第一行是"常用"，
     * 第二行是"后来加的"。拆行不影响任何测试——help 的断言只查包含关系。 */
    static const char *const l1 =
        "help echo ver clear hist set unset stat irq tasks";
    static const char *const l2 =
        "log heap kstress disk fs ls fd cat write rm mkdir exec";
    static const char *const l3 =
        "sys date clock pgd panic";

    shell_out_puts(l1, (unsigned int)shell_str_len(l1, SHELL_OUT_COLS));
    shell_out_puts(l2, (unsigned int)shell_str_len(l2, SHELL_OUT_COLS));
    shell_out_puts(l3, (unsigned int)shell_str_len(l3, SHELL_OUT_COLS));
    return SHELL_RC_OK;
}

/* `hist`：把记着的命令按“旧 -> 新”每条一行写出来，最多 4 条。 */
static int cmd_hist(void)
{
    unsigned int j;

    for (j = 0; j < ST->hist_n; j++) {
        unsigned int oldest = (ST->hist_head + SHELL_HIST - ST->hist_n) % SHELL_HIST;
        unsigned int idx = (oldest + j) % SHELL_HIST;

        shell_out_puts(ST->hist[idx], ST->hist_len[idx]);
    }
    return SHELL_RC_OK;
}

static int cmd_echo(void)
{
    out_puts_args(1);                        /* 把参数原样拼回去（$ 已展开） */
    return SHELL_RC_OK;
}

static int cmd_ver(void)
{
    static const char *const ver = "mini-os shell 0.2";

    shell_out_puts(ver, (unsigned int)shell_str_len(ver, SHELL_OUT_COLS));
    return SHELL_RC_OK;
}

static int cmd_clear(void)
{
    shell_out_reset();                       /* 缓冲清空 */
    shell_con_clear_next();                  /* 用户页也一起抹掉 */
    return SHELL_RC_OK;
}

/* `set`：不带参数列全部（`name=value` 一行一条）；带参数`set <名字> [值...]`。 */
static int cmd_set(void)
{
    unsigned int i;

    if (shell_argc <= 1) {
        /* `set` 单独一条：列出来（空表就什么都不写） */
        for (i = 0; i < SHELL_VARS; i++) {
            char line[SHELL_OUT_COLS];
            unsigned int k = 0, j;

            if (!ST->var_used[i])
                continue;
            for (j = 0; j < SHELL_NAME_MAX && ST->var_name[i][j] && k < SHELL_OUT_COLS; j++)
                line[k++] = ST->var_name[i][j];
            if (k < SHELL_OUT_COLS)
                line[k++] = '=';
            for (j = 0; j < SHELL_VAL_MAX && ST->var_val[i][j] && k < SHELL_OUT_COLS; j++)
                line[k++] = ST->var_val[i][j];
            shell_out_puts(line, k);
        }
        return SHELL_RC_OK;
    }
    if (shell_argc < 2 || shell_argv_len[1] == 0)
        return SHELL_RC_OK;                  /* `set` 后面什么都没有：等同列出 */

    {
        char val[SHELL_VAL_MAX];
        unsigned int k = 0, j;

        for (i = 2; i < shell_argc; i++) {
            if (i > 2 && k < SHELL_VAL_MAX - 1)
                val[k++] = ' ';
            for (j = 0; j < shell_argv_len[i] && k < SHELL_VAL_MAX - 1; j++)
                val[k++] = shell_argv_buf[i][j];
        }
        if (shell_var_set(shell_argv_buf[1], shell_argv_len[1], val, k) < 0)
            return SHELL_RC_VAR;
    }
    return SHELL_RC_OK;
}

static int cmd_unset(void)
{
    if (shell_argc >= 2 && shell_argv_len[1] > 0)
        shell_var_clear(shell_argv_buf[1], shell_argv_len[1]);
    return SHELL_RC_OK;
}


/* 命令名 -> 处理函数。顺序就是 `help` 输出的顺序，改表不用改别处。 */
struct shell_cmd {
    const char *name;
    unsigned int name_len;
    int (*fn)(void);
};

static const struct shell_cmd cmds[] = {
    { "help",  4, cmd_help  },
    { "echo",  4, cmd_echo  },
    { "ver",   3, cmd_ver   },
    { "clear", 5, cmd_clear },
    { "hist",  4, cmd_hist  },
    { "set",   3, cmd_set   },
    { "unset", 5, cmd_unset },
    { "stat",  4, cmd_stat  },
    { "irq",   3, cmd_irq   },
    { "tasks", 5, cmd_tasks },
    { "log",   3, cmd_log   },
    { "heap",  4, cmd_heap  },
    { "kstress", 7, cmd_kstress },
{ "kwait",  5, cmd_kwait  },
    { "disk",  4, cmd_disk  },
    { "fs",    2, cmd_fs    },
    { "ls",    2, cmd_ls    },
    { "fd",    2, cmd_fd    },
    { "pgd",   3, cmd_pgd   },
    { "cat",   3, cmd_cat   },
    { "write", 5, cmd_write },
    { "rm",    2, cmd_rm    },
    { "mkdir", 5, cmd_mkdir },
    { "dev",   3, cmd_dev   },
{ "pipe",  4, cmd_pipe  },
    { "exec",  4, cmd_exec  },
    { "sys",   3, cmd_sys   },
    { "date",  4, cmd_date  },
    { "clock", 5, cmd_clock },
    { "panic", 5, cmd_panic },
};

#define SHELL_CMDS ((unsigned int)(sizeof(cmds) / sizeof(cmds[0])))

/* 把 `$name` 展开进当前参数。返回吃掉了几个源字符（0 = 这里不是变量）。 */
static unsigned int expand_var(const char *line, unsigned int len, unsigned int i,
                               char *dst, unsigned int *k)
{
    unsigned int j = i + 1, nlen = 0;
    int idx;

    if (i >= len || line[i] != '$')
        return 0;                            /* 不是变量：原样当普通字符 */
    while (j < len && shell_var_name_char(line[j]) && nlen < SHELL_NAME_MAX - 1) {
        j++;
        nlen++;
    }
    if (nlen == 0)
        return 0;                            /* 光一个 '$'：原样当普通字符 */
    idx = shell_var_find(line + i + 1, nlen);
    if (idx >= 0) {
        unsigned int v;

        for (v = 0; v < SHELL_VAL_MAX && ST->var_val[idx][v] && *k < SHELL_ARG_MAX - 1; v++)
            dst[(*k)++] = ST->var_val[idx][v];
    }
    return nlen + 1;
}

/* 把一行切成参数。返回参数个数；超过上限返回 -1（不猜，直接报错）；
 * 单个参数超过 SHELL_ARG_MAX 返回 -2 —— 以前是静默截断，会让 `write big <48 字节>`
 * 变成"只写了 23 字节还报 rc=0"，测试里就撞到过，所以改成整行拒掉。 */
static int parse(const char *line, unsigned int len)
{
    unsigned int i = 0, n = 0;
    int too_long = 0;

    shell_argc = 0;
    while (i < len) {
        unsigned int k = 0;
        int quoted = 0;

        while (i < len && (line[i] == ' ' || line[i] == '\t'))
            i++;                             /* 连续空白只当一次分隔 */
        if (i >= len)
            break;
        if (n >= SHELL_ARGC_MAX)
            return -1;                       /* 参数太多：这一行不执行 */
        if (line[i] == '"') {
            quoted = 1;                      /* 引号里空格不切分 */
            i++;
        }
        while (i < len) {
            unsigned int ate;

            if (quoted) {
                if (line[i] == '"') {
                    i++;
                    break;
                }
            } else if (line[i] == ' ' || line[i] == '\t') {
                break;
            }
            ate = expand_var(line, len, i, shell_argv_buf[n], &k);
            if (ate) {
                i += ate;
                continue;
            }
            if (k < SHELL_ARG_MAX - 1)
                shell_argv_buf[n][k++] = line[i];  /* 超长截断，不越界 */
            else
                too_long = 1;                  /* 有过截断：这一行不执行，别让它"看起来对" */
            i++;
        }
        shell_argv_buf[n][k] = '\0';
        shell_argv_len[n] = k;
        n++;
    }
    shell_argc = n;
    return too_long ? -2 : (int)n;
}

/* 判断这一行是不是 `!!`（重跑上一条），是就换成那条命令。
 * 返回 1 = 已替换，0 = 原样；没有历史时返回 -1（调用方报 -4）。 */
static int expand_repeat(const char **line, unsigned int *len)
{
    unsigned int idx;

    if (*len != 2 || (*line)[0] != '!' || (*line)[1] != '!')
        return 0;
    if (ST->hist_n == 0)
        return -1;
    idx = (ST->hist_head + SHELL_HIST - 1) % SHELL_HIST;
    *line = ST->hist[idx];
    *len = ST->hist_len[idx];
    return 1;
}

/* 执行一行。返回值见 SHELL_RC_*。 */
int shell_run(const char *line, unsigned int len)
{
    unsigned int i;
    unsigned int head0, count0;
    int rc, repeated;

    ST->runs++;
    shell_runs_total++;
    shell_con_mark(&head0, &count0);
    repeated = expand_repeat(&line, &len);
    if (repeated < 0) {
        ST->error_count++;
        ST->last_rc = SHELL_RC_NOHIST;
        if (shell_console_on()) {
            static const char *const msg = "!!: no history";

            shell_con_puts("> ", 2);
            shell_con_puts(msg, (unsigned int)shell_str_len(msg, SHELL_OUT_COLS));
            shell_con_newline();
        }
        return ST->last_rc;
    }
    if (shell_console_on()) {
        shell_con_puts("> ", 2);
        shell_con_puts(line, len);
        shell_con_newline();
    }
    rc = parse(line, len);
    if (rc == 0) {
        ST->last_rc = SHELL_RC_EMPTY;        /* 空行：算一次运行，但什么都不做 */
        shell_con_report(head0, count0);
        return ST->last_rc;
    }
    if (repeated == 0)
        hist_record(line, len);              /* `!!` 展开后的那条不再重复记账 */
    if (rc < 0) {
        ST->error_count++;
        if (rc == -2) {
            static const char *const msg = "arg too long (max 23)";

            ST->last_rc = SHELL_RC_LONG;
            if (shell_console_on()) {
                shell_con_puts("> ", 2);
                shell_con_puts(msg, (unsigned int)shell_str_len(msg, SHELL_OUT_COLS));
                shell_con_newline();
            }
        } else {
            ST->last_rc = SHELL_RC_ARGV;
        }
        shell_con_report(head0, count0);
        return ST->last_rc;
    }
    for (i = 0; i < SHELL_CMDS; i++) {
        if (shell_argv_len[0] == cmds[i].name_len &&
            shell_str_eq(shell_argv_buf[0], cmds[i].name, cmds[i].name_len)) {
            stat_inc(STAT_CMD);            /* 只有“真的分派到了”才计数，敲错不算 */
            ST->last_rc = cmds[i].fn();
            shell_con_report(head0, count0);
            return ST->last_rc;
        }
    }
    ST->unknown_count++;
    ST->last_rc = SHELL_RC_UNKNOWN;
    shell_con_report(head0, count0);
    return ST->last_rc;
}

/* 开用户页：显存在 shell/console.c，这里只负责“历史从这一刻开始算”。
 * 只在自检全部打印完之后调用（main 末尾），这样 verify 的第 0 页一个字节都不动。 */
void shell_console_open(void)
{
    ST->hist_head = 0;
    ST->hist_n = 0;                       /* 自检那几行也别进用户的历史 */
    shell_con_open();
}
