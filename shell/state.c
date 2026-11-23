/* shell/state.c —— 持久态、参数缓冲与证据读取。
 * 见 include/shell/state.h：这里是 shell 另外三个文件共享的唯一状态。 */
#include "linux/state.h"

static struct shell_state states[SHELL_STATES];
static unsigned int bound;                /* 当前绑定的 owner 槽（0..SHELL_STATES-1） */

char shell_argv_buf[SHELL_ARGC_MAX][SHELL_ARG_MAX];
unsigned int shell_argv_len[SHELL_ARGC_MAX];
unsigned int shell_argc;                  /* 上一行解出几个参数 */

unsigned int shell_runs_total;

struct shell_state *shell_state(void)
{
    return &states[bound];
}

/* ---- owner 绑定：0..SHELL_STATES-1，越界钳到 0 ---- */
int shell_bind(unsigned int owner)
{
    bound = owner < SHELL_STATES ? owner : 0;
    return (int)bound;
}

unsigned int shell_bound_owner(void)
{
    return bound;
}

/* ---- 没有 libc，自己写两个够用的字符串函数 ---- */
int shell_str_eq(const char *a, const char *b, unsigned int n)
{
    unsigned int i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
        if (!a[i])
            return 1;
    }
    return 1;
}

int shell_str_len(const char *s, unsigned int max)
{
    unsigned int n = 0;

    while (n < max && s[n])
        n++;
    return (int)n;
}

/* ---- 证据接口 ---- */
unsigned int shell_runs(void)      { return ST->runs; }
unsigned int shell_unknown(void)   { return ST->unknown_count; }
unsigned int shell_errors(void)    { return ST->error_count; }
unsigned int shell_last_argc(void) { return shell_argc; }
int          shell_last_rc(void)   { return ST->last_rc; }

/* ---- 命令历史（`hist` / `!!` 的证据）：idx 从最新往前数，0 = 最新 ---- */
unsigned int shell_hist_count(void) { return ST->hist_n; }
const char *shell_hist_line(unsigned int idx)
{
    if (idx >= ST->hist_n)
        return 0;
    return ST->hist[(ST->hist_head + SHELL_HIST - 1 - idx) % SHELL_HIST];
}
unsigned int shell_hist_len(unsigned int idx)
{
    if (idx >= ST->hist_n)
        return 0;
    return ST->hist_len[(ST->hist_head + SHELL_HIST - 1 - idx) % SHELL_HIST];
}

/* ---- 变量表（`set` / `$name` 的证据）：按槽位 0..SHELL_VARS-1 ---- */
unsigned int shell_var_count(void)
{
    unsigned int i, n = 0;

    for (i = 0; i < SHELL_VARS; i++)
        if (ST->var_used[i])
            n++;
    return n;
}
const char *shell_var_name(unsigned int idx)
{
    if (idx >= SHELL_VARS || !ST->var_used[idx])
        return 0;
    return ST->var_name[idx];
}
const char *shell_var_value(unsigned int idx)
{
    if (idx >= SHELL_VARS || !ST->var_used[idx])
        return 0;
    return ST->var_val[idx];
}
