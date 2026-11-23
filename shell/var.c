/* shell/var.c —— 变量表：`set` / `unset` / `$name` 的存放与查找。
 * 见 include/shell/var.h。表挂在 ST 上，所以天然按 owner 隔离。 */
#include "linux/var.h"
#include "linux/state.h"

int shell_var_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

int shell_var_find(const char *name, unsigned int nlen)
{
    unsigned int i;

    if (nlen == 0 || nlen >= SHELL_NAME_MAX)
        return -1;
    for (i = 0; i < SHELL_VARS; i++) {
        if (!ST->var_used[i])
            continue;
        if ((unsigned int)shell_str_len(ST->var_name[i], SHELL_NAME_MAX) == nlen &&
            shell_str_eq(ST->var_name[i], name, nlen))
            return (int)i;
    }
    return -1;
}

int shell_var_set(const char *name, unsigned int nlen,
                  const char *val, unsigned int vlen)
{
    unsigned int i, slot;

    if (nlen == 0 || nlen >= SHELL_NAME_MAX)
        return -1;
    if (vlen > SHELL_VAL_MAX - 1)
        vlen = SHELL_VAL_MAX - 1;
    slot = SHELL_VARS;
    for (i = 0; i < SHELL_VARS; i++) {
        if (ST->var_used[i]) {
            if ((unsigned int)shell_str_len(ST->var_name[i], SHELL_NAME_MAX) == nlen &&
                shell_str_eq(ST->var_name[i], name, nlen))
                slot = i;                        /* 已有：覆盖 */
        } else if (slot == SHELL_VARS) {
            slot = i;                            /* 顺带记住第一个空位 */
        }
    }
    if (slot == SHELL_VARS)
        return -1;                               /* 表满 */
    for (i = 0; i < nlen; i++)
        ST->var_name[slot][i] = name[i];
    ST->var_name[slot][nlen] = '\0';
    for (i = 0; i < vlen; i++)
        ST->var_val[slot][i] = val[i];
    ST->var_val[slot][vlen] = '\0';
    ST->var_used[slot] = 1;
    return 0;
}

void shell_var_clear(const char *name, unsigned int nlen)
{
    int idx = shell_var_find(name, nlen);

    if (idx >= 0) {
        ST->var_used[idx] = 0;
        ST->var_name[idx][0] = '\0';
        ST->var_val[idx][0] = '\0';
    }
}
