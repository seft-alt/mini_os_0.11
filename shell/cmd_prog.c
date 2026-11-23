/* 程序与地址空间类命令：pgd / exec / sys。
 *
 * 由 shell/shell.c 拆分而来（纯搬运，逻辑未改）。
 * ============================================================== */
#include "linux/shell.h"
#include "linux/state.h"
#include "linux/console.h"
#include "linux/fmt.h"
#include "linux/cmds.h"
#include "linux/exec.h"
#include "linux/mosfs.h"
#include "linux/pgdir.h"
#include "linux/tss.h"
#include "linux/exception.h"
#include "linux/kv.h"
#include "linux/syscall.h"

/* `pgd`：每任务页目录的一行证据。
 * 打 s1..s4 而不是 s0..s4：slot 0 恒为 0（main 用内核页目录），
 * 但 5 个槽全打一行会超过 79 列——少一列恒为空的值，换来一行装得下。
 *   sN=页目录物理地址（0x 前缀，6 位；0 = 这个槽位没有私有表）
 *   sw=调 pgdir_switch 的次数（只在真的换表时涨，idle 到 idle 不动）
 *   us=此刻有几个槽位合计几格 US=1（跑完应当回 0）
 *   mp=累计放行次数（set_user 成功）
 *   fl=累计放行失败次数（mp 不动时，看 fl 就知道"没调"还是"调了但失败"）
 *   fe=ring3 最近一次缺页的错误码（0x7 = P|W|U = 写只读页，3 = 读不存在的页）
 *   tc=TSS.cr3，应当永远等于 CPU 此刻在用的表（空闲时 = 0x000000 = 内核表）
 *
 * 拆成两行是因为一行装不下：s1..s4 已经占 44 列，再加五个计数就超 79。
 * 第二行用 `pgdc` 打头，和 `pgd` 一样是"pgdir 计数"的缩写。 */
int cmd_pgd(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int k, i, us = 0;

    k = put_lit(line, 0, "pgd");
    for (i = 1; i < TASK_SLOTS; i++) {
        line[k++] = ' ';
        line[k++] = 's';
        line[k++] = (char)('0' + (int)i);
        line[k++] = '=';
        k += hex6(line + k, (unsigned long)pgdir_of((int)i));
    }
    shell_out_puts(line, k);

    for (i = 0; i < TASK_SLOTS; i++)
        us += pgdir_user_pages((int)i);
    k = put_lit(line, 0, "pgdc sw=");
    k += udec(line + k, (unsigned long)pgdir_switches());
    k = put_lit(line, k, " us=");
    k += udec(line + k, (unsigned long)us);
    k = put_lit(line, k, " mp=");
    k += udec(line + k, (unsigned long)pgdir_maps());
    k = put_lit(line, k, " fl=");
    k += udec(line + k, (unsigned long)pgdir_fails());
    k = put_lit(line, k, " fe=");       /* ring3 最近一次缺页的错误码（写保护看这里） */
    k += hex6(line + k, ring3_fault_err);
    k = put_lit(line, k, " tc=");       /* hex6 自带 0x，这里不要再写一遍 */
    k += hex6(line + k, (unsigned long)tss_cr3());
    shell_out_puts(line, k);

    /* 同一条证据再走一遍串口（机器可读）。屏幕那两行是给人看的，脚本判的是
     * 这一行——`KV pgdir s1=.. sw=.. us=..` 的字段口径与屏幕完全一致，
     * 所以 pgdir.sh 从"抓屏 + 正则"迁过来时判据字符串不用重写。 */
    {
        char key[4];

        kv_begin("pgdir");
        for (i = 1; i < TASK_SLOTS; i++) {
            key[0] = 's';
            key[1] = (char)('0' + (int)i);
            key[2] = '\0';
            kv_hex(key, pgdir_of((int)i));
        }
        kv_dec("sw", pgdir_switches());
        kv_dec("us", us);
        kv_dec("mp", pgdir_maps());
        kv_dec("fl", pgdir_fails());
        kv_hex("fe", ring3_fault_err);
        kv_hex("tc", tss_cr3());
        kv_end();
    }
    return SHELL_RC_OK;
}

/* `exec <名字>`：把盘上的程序装进用户页、在 ring3 跑一趟。
 * 打印三个数各有其用途：rc 说明这一趟成没成；magic 是程序**自己**写下的指纹
 * （不是内核替它填的）；val 是程序算出来的结果（1..8 求和 = 36）。
 * 没有 magic 就别信 val——那可能只是我们清结果区留下的 0xDEAD0000。 */
int cmd_exec(void)
{
    char name[FS_NAME_MAX + 1];
    char line[SHELL_OUT_COLS];
    unsigned long magic = 0, value = 0, ex = 0;
    unsigned int k = 0, j;
    unsigned int sc0, sd0, sc, sd;
    int rc;

    if (shell_argc < 2) {
        shell_out_puts("exec <file>", 11);
        return SHELL_RC_OK;
    }
    for (j = 0; j < shell_argv_len[1] && j < FS_NAME_MAX; j++)
        name[j] = shell_argv_buf[1][j];
    name[j] = '\0';

    sc0 = sys_calls();
    sd0 = sys_denied();
    {
        /* 程序拿到的 argv[] 是 shell 这一行的第 2..N 个词：
         * `exec uprog a b` → argc=3, argv={"uprog","a","b"}（标准习惯）。 */
        const char *av[EXEC_ARGV_MAX];
        int ac = 0, a;

        for (a = 1; a < (int)shell_argc && ac < EXEC_ARGV_MAX; a++)
            av[ac++] = shell_argv_buf[a];
        rc = exec_run(name, ac, av, &magic, &value);
    }
    ex = exec_exit_code();
    /* syscall 的账按"这一趟"算：跑前跑后各读一次取差值。
     * 累计值会把自检里那些过门的调用算进来，"这一趟打了几次 syscall"就看不出来了。 */
    sc = sys_calls() - sc0;
    sd = sys_denied() - sd0;

    line[k++] = 'e'; line[k++] = 'x'; line[k++] = 'e'; line[k++] = 'c';
    line[k++] = ' ';
    for (j = 0; name[j] != '\0' && k < 24; j++)
        line[k++] = name[j];
    line[k++] = ' ';
    line[k++] = 'r'; line[k++] = 'c'; line[k++] = '=';
    if (rc == 0) {
        line[k++] = '0';
    } else {
        line[k++] = 'E';
        line[k++] = (char)('1' + (-rc - 1));
    }
    line[k++] = ' ';
    line[k++] = 'm'; line[k++] = 'a'; line[k++] = 'g'; line[k++] = '=';
    k += hex8(line + k, magic);
    line[k++] = ' ';
    line[k++] = 'v'; line[k++] = 'a'; line[k++] = 'l'; line[k++] = '=';
    k += udec(line + k, value);
    line[k++] = ' ';
    line[k++] = 'e'; line[k++] = 'x'; line[k++] = '=';
    if (ex == 0xFFFFFFFFUL)
        line[k++] = '-';            /* 程序没调 exit（比如用 0xFF 老旁路走的） */
    else
        k += udec(line + k, ex);
    line[k++] = ' ';
    line[k++] = 's'; line[k++] = 'y'; line[k++] = '=';
    k += udec(line + k, sc);
    line[k++] = '/';
    k += udec(line + k, sd);
    line[k++] = ' ';
    line[k++] = 't'; line[k++] = '=';       /* 跑在哪个任务槽位 */
    k += udec(line + k, (unsigned long)exec_last_slot());
    line[k++] = ' ';
    line[k++] = 'd'; line[k++] = '=';       /* 那个任务的死因（0=return 1=exit 2=guard） */
    k += udec(line + k, (unsigned long)exec_last_death());
    line[k++] = ' ';
    line[k++] = 'n'; line[k++] = '=';
    k += udec(line + k, exec_count());
    line[k++] = '/';
    k += udec(line + k, exec_failures());
    shell_out_puts(line, k);
    /* 第二行：pg（只在跨页时出现）、u=、ep=。主行加上 ep= 会到 81 列
     * （79 列上限，实测被截），所以 ep 与 u 一起挪到这一行；
     * elf.sh 的 `sed -n 's/.* ep=0x\(..\)$/'` 仍能命中——ep 依旧在行尾。 */
    {
        char l2[40];
        unsigned int m = 0;

        l2[m++] = 'e'; l2[m++] = 'x'; l2[m++] = 'e'; l2[m++] = 'c'; l2[m++] = ' ';
        if (exec_last_pages() > 1) {
            l2[m++] = 'p'; l2[m++] = 'g'; l2[m++] = '=';
            m += udec(l2 + m, exec_last_pages());
            l2[m++] = ' ';
        }
        l2[m++] = 'u'; l2[m++] = '=';
        m += udec(l2 + m, exec_last_user_pages());
        l2[m++] = ' ';
        /* lh=：这几页里有几笔租约真的改记到了任务头上（u= 是几页）。 */
        l2[m++] = 'l'; l2[m++] = 'h'; l2[m++] = '=';
        m += udec(l2 + m, exec_last_handed());
        l2[m++] = ' '; l2[m++] = 'j'; l2[m++] = 'b'; l2[m++] = '=';
        m += udec(l2 + m, (unsigned long)exec_slots_used());   /* 该回到 0 */
        l2[m++] = ' ';
        /* asu=：地址空间池还占着几格（和 jb= 一样，跑完该回到 0） */
        l2[m++] = 'a'; l2[m++] = 's'; l2[m++] = 'u'; l2[m++] = '=';
        m += udec(l2 + m, (unsigned long)exec_live_aspaces());
        l2[m++] = ' ';
        l2[m++] = 'e'; l2[m++] = 'p'; l2[m++] = '=';   /* 入口相对页头：0=裸二进制 */
        m += hex4(l2 + m, exec_last_entry());
        shell_out_puts(l2, m);
    }

    /* 串口那一路（机器可读）：字段与上面两行同口径，另外把 sy/sd 拆成两个键
     * （屏幕上是 `sy=2/1` 这种"一对"，脚本要再切一次 `/`，KV 里不必）。 */
    kv_begin("exec");
    kv_str("name", name);
    kv_code("rc", rc);
    kv_hex("mag", magic);
    kv_dec("val", value);
    if (ex == 0xFFFFFFFFUL)
        kv_str("ex", "-");          /* 程序没调 exit（老旁路 0xFF 走掉的） */
    else
        kv_dec("ex", ex);
    kv_dec("sy", sc);
    kv_dec("sd", sd);
    kv_dec("t", (unsigned long)exec_last_slot());
    kv_dec("d", (unsigned long)exec_last_death());
    kv_dec("n", exec_count());
    kv_dec("nf", exec_failures());
    kv_dec("pg", exec_last_pages());
    kv_dec("u", exec_last_user_pages());
    kv_dec("lh", exec_last_handed());
    kv_dec("jb", (unsigned long)exec_slots_used());
    kv_dec("asu", exec_live_aspaces());
    kv_hex("ep", exec_last_entry());
    /* 最近一次 ring3 缺页的错误码（写保护/越权看这里）。放在这条记录里而不是
     * 只靠后面的 `pgd`：`exec` 的现场与这条码是同一趟，中间不必再敲一条命令。 */
    kv_hex("fe", ring3_fault_err);
    kv_end();
    return SHELL_RC_OK;
}

/* `sys`：syscall 层的账。过门几次、被拒几次、写出去多少字节、最近一次是几号、
 * 最近一次的退出码。exit 用 SYS_EXIT_NONE 表示"还没有程序退过"。 */
int cmd_sys(void)
{
    char line[SHELL_OUT_COLS];
    unsigned int k = 0;
    unsigned long nr = sys_last_nr();
    unsigned long ex = sys_exit_code();

    line[k++] = 'c'; line[k++] = '=';  k += udec(line + k, sys_calls());
    line[k++] = ' '; line[k++] = 'd'; line[k++] = '='; k += udec(line + k, sys_denied());
    line[k++] = ' '; line[k++] = 'b'; line[k++] = '='; k += udec(line + k, sys_bytes_out());
    line[k++] = ' '; line[k++] = 'n'; line[k++] = '='; k += udec(line + k, nr);
    line[k++] = ' '; line[k++] = 'x'; line[k++] = '=';
    if (ex == 0xFFFFFFFFUL)
        line[k++] = '-';
    else
        k += udec(line + k, ex);
    shell_out_puts(line, k);
    return SHELL_RC_OK;
}
