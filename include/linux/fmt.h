/* shell 的格式化 helper —— 定义在 shell/fmt.c。
 *
 * 原来是 shell.c 里的 static，拆文件后跨单元可见，签名与语义未变。
 * 注意这里只有“格式化”，没有解析：`parse_int` 只服务 `clock skew`，
 * 已随 clock/date 搬进 shell/cmd_time.c 做 static。 */
#ifndef MINI_OS_SHELL_FMT_H
#define MINI_OS_SHELL_FMT_H

unsigned int hex2b(char *dst, unsigned int v);
unsigned int hex3(char *dst, unsigned long v);
unsigned int hex4(char *dst, unsigned int v);
unsigned int hex6(char *dst, unsigned long v);
unsigned int hex8(char *dst, unsigned long v);
unsigned int udec(char *dst, unsigned long v);
unsigned int put_lit(char *dst, unsigned int k, const char *s);
unsigned int put_rc(char *dst, unsigned int k, int rc);
unsigned int put_s(char *dst, const char *s);
unsigned int u3(char *dst, unsigned int v);
unsigned int sdec(char *dst, int v);

#endif /* MINI_OS_SHELL_FMT_H */
