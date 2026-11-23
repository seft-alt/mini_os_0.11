/* ==============================================================
 * shell 的变量表：`set` / `unset` / `$name` 展开的存放处
 *
 * 名字 ≤7 字符（字母数字下划线），值 ≤23 字符，6 格，满了 `set` 报 -5。
 * 值里允许空格（`set path a b` 把 "a b" 拼进去），但不允许再展开 `$`——
 * 不做递归展开，避免 `$a` 里再套 `$b` 这种“展开到没完”。
 * 表本身挂在 `struct shell_state` 上，所以变量天然跟着 owner 走。
 * ============================================================== */
#ifndef MINI_OS_SHELL_VAR_H
#define MINI_OS_SHELL_VAR_H

int  shell_var_name_char(char c);
int  shell_var_find(const char *name, unsigned int nlen);
int  shell_var_set(const char *name, unsigned int nlen,
                   const char *val, unsigned int vlen);
void shell_var_clear(const char *name, unsigned int nlen);

#endif /* MINI_OS_SHELL_VAR_H */
