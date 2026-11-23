/* 搬到别的文件里的命令（分派表按名字引用，不关心实现在哪个文件）。
 *
 * 签名统一 `int cmd_xxx(void)`：参数从 shell_argc / shell_argv_buf 读，
 * 返回值是 SHELL_RC_*（见 shell.c）。留在 shell.c 里的内建命令
 * （help/hist/echo/ver/clear/set/unset）仍是 static，不在这里声明。 */
#ifndef MINI_OS_SHELL_CMDS_H
#define MINI_OS_SHELL_CMDS_H

/* shell/cmd_fs.c：文件系统与磁盘 */
int cmd_disk(void);
int cmd_fs(void);
int cmd_ls(void);
int cmd_cat(void);
int cmd_fd(void);
int cmd_write(void);
int cmd_rm(void);
int cmd_mkdir(void);
int cmd_dev(void);
int cmd_pipe(void);                 /* 管道：读端写端各占一格，环 512 字节 */

/* shell/cmd_prog.c：程序与地址空间 */
int cmd_pgd(void);
int cmd_exec(void);
int cmd_sys(void);

/* shell/cmd_info.c：观测 */
int cmd_stat(void);
int cmd_irq(void);
int cmd_tasks(void);
int cmd_log(void);
int cmd_heap(void);
int cmd_kstress(void);
int cmd_kwait(void);                /* 等事件：两个 worker 用信箱 ping-pong 16 轮 */
int cmd_clock(void);
int cmd_date(void);
int cmd_panic(void);

#endif /* MINI_OS_SHELL_CMDS_H */
