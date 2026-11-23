/* fs/exec.c —— 把文件装进用户页，交给一个任务去跑（细节见 include/fs/exec.h）
 *
 * 这一版和上一版的区别只有一句话：**跑的是一段 ring3，但不再是"exec 自己 iret 进去"**。
 * 之前 exec 在主上下文里同步下潜：它自己 user_enter、等程序回来、再收拾，期间
 * 连调度都不发生（IF=0）。现在代码页与用户栈页被打包成一个 job，交给
 * task_spawn_owned 建的新任务；任务里跑 ring3，跑完从 task_return_stub 退出。
 * **页归任务**：spawn 之前用 task_user_offer 交接，任务一死由调度器在换栈之后
 * 逐页关租约、还页；exec 只负责"等这件事发生"。
 *
 * 等待是"睡在 hlt 里"而不是死转：cpu_hlt() 让出 CPU，下一个 tick 进来由
 * schedule 选中那个任务；任务死掉后 active 归 0，我们回到 hlt 后面，
 * 再用 task_reclaim() 让 release_pending() 把僵尸栈与 dtor 交接掉。
 * 所以这个"阻塞"其实是抢占式的：主的循环里一直在跑调度。
 *
 * 三层顺序仍然不能乱（见 include/fs/exec.h）：先要页 → 再拷 → 最后放行。 */
#include "linux/exec.h"
#include "linux/mosfs.h"
#include "linux/elf.h"
#include "linux/page.h"
#include "linux/aspace.h"
#include "linux/lease.h"
#include "linux/task.h"
#include "linux/tune.h"
#include "linux/syscall.h"
#include "asm/io.h"
#include "linux/printk.h"
#include "linux/panic.h"
#include "linux/string.h"
#include "linux/pgdir.h"

/* boot/intr.asm 里的两样东西：进 ring3 的跳板、门处理体改写的"回内核"落点 */
extern void user_enter(unsigned long entry, unsigned long user_esp);

/* 门窗口的大小与布局：定义搬到了 include/fs/exec.h（用户程序要按同一组常量
 * 算偏移，写在 .c 里它看不见）。这里只留一句老注释作路标：
 *   [esp0-256, esp0-128)  fd 的 I/O 缓冲（这一版新增）
 *   [esp0-128, esp0-112)  要 write 的字符串（16 字节够放一行短句）
 *   [esp0-24,  esp0-16)   指纹与结果值
 *   [esp0-16,  esp0)      调用门请求结构体（四个字）
 * 把窗口开得比"刚够一个结构体"大一点，是为了让 write 的参数也过同一套校验：
 * 用户指针只要还在这一段里，内核就敢读。 */

/* 一次 exec 的全部家当。**每槽位一份**：以前是一个静态单槽，理由是"exec 串行、
 * 主上下文在等这一趟，期间不会有第二趟"。这个理由只在"只有一个调用者"时成立——
 * 一旦支持后台执行/多程序并发，两趟就会抢同一个 done 标志和同一份结果区，
 * 现象是"第二趟立刻返回，读到的却是第一趟的结果"。
 *
 * 现在表是按槽位分的（跟 tasks[] 一一对应），`busy` 表示这一格正在用。
 * 仍然**不分配**（静态表），因为内核堆还没有"可回收对象"这一类东西；
 * 槽位表就这么大，用满就明确失败（返回 -7），不静默排队。 */
struct exec_job {
    struct aspace *as;              /* 这一趟的地址空间：代码页/栈页/入口/esp0 都在里面 */
    unsigned long magic;            /* 程序写下的指纹 */
    unsigned long value;            /* 程序算出的结果 */
    unsigned long exit;             /* 程序自己交回的退出码 */
    int slot;                       /* 跑它的任务槽位（0 = 没跑起来） */
    int death;                      /* 死因（TASK_DEATH_*，dtor 记的） */
    unsigned int entry_rel;         /* 入口相对页头：裸二进制 = 0，ELF = e_entry */
    /* 每页对 ring3 可写吗：1 = 可写，0 = 只读（写保护）。裸二进制整段填 1（
     * 老路径行为不变）；ELF 由 elf_load_map 按 PF_W 段算出——纯代码段
     * （ld 的 FLAGS(5)）整段 0，于是"用户写自己的代码页"会 #PF → 越权 → 任务死。 */
    unsigned char wpage[EXEC_MAX_PAGES];
    int done;                       /* dtor 跑完 = 1 */
    int busy;                       /* 这一格正被一趟 exec 占着 */
};

static struct exec_job jobs[TASK_SLOTS];
static int last_job = -1;           /* 最近一趟用的是哪一格（证据接口按它取值） */
static unsigned int n_exec, n_fail, elf_loaded;
/* 页数另存一份：dtor 会把 aspace 整格归还（里面的 pages 跟着清 0，表示
 * "页已经还了"），而"这一趟要了几页"这个事实要留到 shell 打证据的时候。 */
static unsigned int last_pages;
/* 最近一趟有几笔租约真的改记到任务头上（= "页和账一起归任务"的可数证据）。
 * 小于 last_pages+1 说明有页没交接成功，还挂在 KERNEL 名下靠 ttl 兜底。 */
static unsigned int last_handed;

/* 任务入口：这一段跑在 ring0 的**任务栈**上，它只做"下潜一次、把结果搬回来"。
 * 页的回收不在这里：用户页归任务，由调度器死后统一收；任务自己的栈页也是
 * 调度器换栈之后才还的。这样"谁放了什么"只有一处。 */
static void exec_task_body(void *arg)
{
    struct exec_job *j = (struct exec_job *)arg;
    struct aspace *as;
    volatile unsigned long *res;

    /* 走到这里还没有对象，说明 dtor 已经跑过（不可能）或装配段漏了赋值：
     * 继续往下 deref 就是"空指针写 0x0"，比停机更难查。 */
    ASSERT(j && j->as, "exec: task body without aspace");
    as = j->as;

    as->esp0 = (unsigned long)as->stack + PAGE_SIZE_BYTES;
    j->entry_rel = as->entry - as->code;
    res = (volatile unsigned long *)as->esp0;

    /* 用户页**在这个任务自己的页目录**里放行。这一句是隔离的另一半：
     * 租约（mm/lease.c）已经在内核页目录里给这几页标了 U/S，那是"ring3 能
     * 访问"的老开关；但每任务页目录上线之后，真正决定"谁能进"的是**各自
     * 私有 PT0 里那一格**——别人的表里根本没有这一格，所以别人的任务
     * 就算拿到同一个物理地址也进不来。
     * 顺序也必须是这样：先放行、再 user_enter（后者才装这张表）。 */
    {
        unsigned int i;

        for (i = 0; i < as->pages; i++)
            pgdir_set_user_page(j->slot, as->code + i * PAGE_SIZE_BYTES, 1,
                                j->wpage[i]);
        pgdir_set_user(j->slot, as->stack, 1);       /* 栈永远可写 */
    }

    /* 窗口只在这个任务跑的时候开着；它一回来就关 */
    tune_gate_user_window(as->esp0 - EXEC_WINDOW, as->esp0);
    res[-6] = 0xDEAD0000UL;         /* 哨兵：程序没写指纹就还是它 */
    res[-5] = 0xDEAD0000UL;
    res[-4] = 0;
    res[-3] = 0;

    user_enter((unsigned long)as->entry, as->esp0);
    /* user_enter 用 iret 进 ring3、eflags 只留 bit1，回来时 IF 还是 0，这里必须自己开。
     * 从 user_enter 回来这件小事里藏着一条硬约束：user_enter / user_return_stub
     * 必须把 ebx/esi/edi 原样还回来（见 boot/intr.asm 的注释）。这里 `j` 很可能
     * 就住在 esi 里，而 ring3 那段代码会把它当自己的寄存器改掉——漏了这一步，
     * `j->magic` 就写到 argv 区去了，症状是 magic 读回 0、而 rc=E5。 */
    cpu_sti();
    tune_gate_user_window(0, 0);

    j->magic = res[-6];
    j->value = res[-5];
    j->exit = sys_exit_code();
    /* 从这里 return：落到 task_return_stub → task_die(RETURN) → dtor 收拾 */
}

/* 任务死后由调度器在**换好的栈**上调用（IF 可能已清，不能睡、不能 spawn）：
 * 只记死因、把 done 置上让 exec 的等待循环醒。
 *
 * 页的回收**不在这里**了：这几页从 spawn 那一刻起就归任务（task_user_offer
 * 交接给 task_t 的 ucode/upages/ustack），任务一死由调度器在
 * release_pending 里逐页关租约、还页。这样"谁放了什么"只有一条线：
 * 谁的任务，谁的页——exec 失败路径自己收，成功之后就不许再碰。 */
static void exec_task_dtor(void *arg, int reason)
{
    struct exec_job *j = (struct exec_job *)arg;

    j->death = reason;
    /* 私有页表里的 US 位先收掉，再把地址空间对象归还。
     * 顺序反了也能跑（对象里还留着 slot），但"先收权限、后还对象"才是
     * 那条不变量该有的方向：权限不该比它保护的页活得更久。 */
    pgdir_clear_user(j->slot);
    /* 页已经不归我了：对象里那几个数字现在只是“上一趟曾用过哪几页”，
     * 留着指针才会误用。整格对象在这里归还——这是“成功之后所有权
     * 归任务、由 dtor 收尾”那半条契约唯一的执行点。 */
    aspace_free(j->as);
    j->as = 0;
    j->done = 1;
}

/* 把已经要来的页与已经开上的租约全部退回去：失败路径只有这一处清理逻辑，
 * 免得每条 return 前各写一遍（漏一条就是漏一页）。 */
static void exec_unwind(unsigned int code, unsigned int pages,
                         unsigned int opens, unsigned int stack)
{
    unsigned int i;

    for (i = 0; i < opens; i++)
        page_lease_close(PAGE_LEASE_OWNER_KERNEL, code + i * PAGE_SIZE_BYTES);
    if (stack) {
        page_lease_close(PAGE_LEASE_OWNER_KERNEL, stack);
        page_free(stack);
    }
    if (code)
        page_free_run(code, pages);
}

/* 把 argc/argv 摆进用户栈顶那 128 字节窗口（布局见 include/fs/exec.h）。
 *
 * 在 spawn **之前**做：这时栈页还归 exec（恒等映射直接写），写坏了只脏自己的页，
 * 而且不用再担心"任务已经在跑、参数还没拷完"这种窗口。拷完就可以不管了。
 *
 * 用的是 lib/string.c 的 strlen/memcpy——参数拷贝是这两个函数在内核里的第一个
 * 真实调用点，string.o 也就有了回归背书（exec-argv 那条测试会走过它）。 */
static void exec_build_args(unsigned long esp0, int argc, const char *const *argv)
{
    unsigned long *ptrs = (unsigned long *)(esp0 - 56);   /* [esp0-56,  esp0-44) */
    int i;

    if (!argv || argc < 0)
        argc = 0;
    if (argc > EXEC_ARGV_MAX)
        argc = EXEC_ARGV_MAX;

    for (i = 0; i < argc; i++) {
        char *slot = (char *)(esp0 - 128 + (unsigned long)i * EXEC_ARG_STRIDE);
        const char *src = argv[i] ? argv[i] : "";
        unsigned int len = strlen(src);

        if (len > EXEC_ARG_LEN)
            len = EXEC_ARG_LEN;                /* 截断：名字太长不该让整趟失败 */
        memcpy(slot, src, len);
        slot[len] = '\0';
        ptrs[i] = (unsigned long)slot;
    }
    /* 没用到的那几格写 0：程序按 argc 循环，但"窗口里没有残值"更好查 */
    for (; i < EXEC_ARGV_MAX; i++)
        ptrs[i] = 0;
    *(unsigned long *)(esp0 - 44) = (unsigned long)argc;
}

/* 一趟 exec 的**装配段**（真身）。**参数换成"哪一格 job"**：所有状态都从 j 走，
 * 不再碰任何全局单槽——这是"两趟并发时状态不串"的结构前提。
 * 失败路径只管把页/租约退回去；对象（aspace）的归还在外面那一处，
 * `busy` 的清理再外面一层。三层各管一件事，谁也都只有一处。 */
static int exec_stage(struct exec_job *j, const char *name, int argc,
                      const char *const *argv,
                      unsigned long *out_magic, unsigned long *out_value)
{
    struct aspace *as = j->as;
    unsigned int ino = 0, size = 0, code = 0, stack = 0, entry = 0;
    unsigned int scratch = 0, pages = 1, need, head, i, opens = 0, span;
    unsigned int handed = 0;
    const unsigned char *peek;
    int rc, slot, sok;

    if (out_magic)
        *out_magic = 0;
    if (out_value)
        *out_value = 0;

    j->magic = j->value = 0;
    j->exit = SYS_EXIT_NONE;
    j->slot = 0;
    j->death = TASK_DEATH_EXIT;
    j->done = 0;
    j->entry_rel = 0;
    last_pages = 0;
    last_handed = 0;

    if (!fs_lookup(name, &ino, &size) || size == 0) {
        n_fail++;
        return -1;
    }
    if (size > EXEC_MAX_BYTES) {
        n_fail++;
        return -2;
    }

    /* 先读文件头进一页临时缓冲：**要几页得在要页之前定下来**，而 ELF 的
     * p_memsz 可能比文件本身大（.bss）。这一步只读第一页，程序头表必须在这一页内
     * （ld 产出的正常 ELF 都在最前面），否则按"装不进去"处理。 */
    scratch = page_alloc();
    if (!scratch) {
        n_fail++;
        return -3;
    }
    head = size < PAGE_SIZE_BYTES ? size : PAGE_SIZE_BYTES;
    rc = fs_read(ino, 0, (void *)scratch, head);
    if (rc <= 0 || (unsigned int)rc != head) {
        page_free(scratch);
        n_fail++;
        return -4;
    }

    peek = (const unsigned char *)scratch;
    need = size;
    if (elf_probe(peek, head)) {
        span = elf_span(peek, head);
        if (span == 0) {                /* 认得出是 ELF、却没有一个能算的 PT_LOAD */
            page_free(scratch);
            n_fail++;
            return -6;
        }
        if (span > need)
            need = span;
    }
    pages = (need + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
    if (pages > EXEC_MAX_PAGES) {
        page_free(scratch);
        n_fail++;
        return -2;
    }

    code = page_alloc_run(pages);
    stack = page_alloc();
    if (!code || !stack) {
        exec_unwind(code, pages, 0, stack);
        page_free(scratch);
        n_fail++;
        return -3;
    }

    /* 拷：整段读进那几页（多页时 fs_read 只是按块顺序拷，跨页不是它的事） */
    rc = fs_read(ino, 0, (void *)code, size);
    if (rc <= 0 || (unsigned int)rc != size) {
        exec_unwind(code, pages, 0, stack);
        page_free(scratch);
        n_fail++;
        return -4;
    }
    page_free(scratch);                 /* 临时页用完即还，不留到收尾 */
    scratch = 0;

    /* 格式分派：认得出 ELF 就按段搬、用 e_entry；认不出就仍当裸二进制（从页头跑）。
     * 这一步在"放行"之前——页这时还是内核私有的，搬错了也只脏了自己的页。
     * 权限图同一趟算出：裸二进制默认整段可写（老路径一个字节都不变），
     * ELF 的页权限由 PF_W 决定，纯代码段会被标成只读。 */
    for (i = 0; i < pages; i++)
        j->wpage[i] = 1;
    if (elf_probe((const unsigned char *)code, size)) {
        if (elf_load_map((const unsigned char *)code, size, (unsigned char *)code,
                         pages * PAGE_SIZE_BYTES, &entry, j->wpage, pages) != 0) {
            exec_unwind(code, pages, 0, stack);
            n_fail++;
            return -6;
        }
        /* elf_load 给的是"段区间内的偏移"，真正要跳的是"页基址 + 偏移" */
        entry += code;
        elf_loaded++;
    } else {
        entry = code;                   /* 裸二进制：老路径，一个字节都不变 */
    }

    /* 放行（打完这趟才由 dtor 关）。顺序仍是"先要页、再拷、最后放行"。
     * 多页时一页一笔租约——租约表一共 8 槽，EXEC_MAX_PAGES + 1 页栈刚好够。 */
    for (i = 0; i < pages; i++) {
        if (!page_lease_open(PAGE_LEASE_OWNER_KERNEL, code + i * PAGE_SIZE_BYTES, 500))
            break;
        opens++;
    }
    sok = page_lease_open(PAGE_LEASE_OWNER_KERNEL, stack, 500);
    if (opens != pages || !sok) {       /* 租约表满了：整段退回去，不带着半个放行往下跑 */
        exec_unwind(code, pages, opens, stack);
        n_fail++;
        return -3;
    }

    as->code = code;
    as->pages = pages;
    as->stack = stack;
    as->entry = entry;
    as->esp0 = 0;                       /* 栈顶由任务自己算（在 body 里） */
    last_pages = pages;
    /* 参数在这里就摆好：这时栈页还归 exec，写坏了也不会伤到谁 */
    exec_build_args((unsigned long)stack + PAGE_SIZE_BYTES, argc, argv);

    /* 任务要真被切上场，调度器得是开着的：自检收尾时它是关的（active=0）。
     * 这里显式打开，任务死光之后 schedule 会自己把 active 归 0。 */
    task_sched_on();
    /* 页归任务：在 spawn 之前交接。task_create 会把它们记进新槽位，
     * 于是"任务已经在跑、页还没记上"这个窗口根本不存在。 */
    task_user_offer(code, pages, stack);
    slot = task_spawn_owned(exec_task_body, j, exec_task_dtor, 2, 0);
    if (!slot) {
        /* 建不起来：页还没交给谁，交接也作废，这里自己收 */
        task_user_offer(0, 0, 0);
        exec_unwind(code, pages, pages, stack);
        n_fail++;
        return -3;
    }
    j->slot = slot;
    as->slot = slot;                    /* 对象知道自己在哪个槽位上跑 */

    /* 账也跟着页走：spawn 之后这几页归任务了，租约一起改记到槽位上，
     * 并且改成**永不到期**。理由很实：ring3 一旦放开 IF，用户态跑久了
     * 就会让内核名下的 ttl 一路递减到 0，然后在自己的页上把 U/S 收走——
     * 那是"页还开着"和"页已经关掉"混在一起的经典翻车。归了任务之后，
     * 回收的唯一触发点就是"这个槽位死了"，由 release_pending 调
     * page_lease_release_owner 收 U/S。
     * 接收方借用表满时 handoff 返回 0，那一笔留在内核名下靠 ttl 兜底。 */
    for (i = 0; i < pages; i++)
        handed += page_lease_handoff(PAGE_LEASE_OWNER_KERNEL, (unsigned int)slot,
                                     code + i * PAGE_SIZE_BYTES);
    handed += page_lease_handoff(PAGE_LEASE_OWNER_KERNEL, (unsigned int)slot, stack);
    last_handed = handed;

    /* 等：不空转，hlt 一次醒一次。每次醒来先让 release_pending 把僵尸栈与
     * dtor 交接掉——没有这一句，dtor 永远不会跑（active 已经是 0 了）。 */
    while (!j->done) {
        task_reclaim();
        cpu_hlt();
    }

    if (out_magic)
        *out_magic = j->magic;
    if (out_value)
        *out_value = j->value;


    if (j->magic != EXEC_MAGIC) {
        n_fail++;
        return -5;
    }

    n_exec++;
    return 0;
}

/* 对象层：给这一趟造一格地址空间 → 装配 → **失败就把对象还回去**。
 *
 * 为什么归还只写在这里：装配段（exec_stage）有十几条失败 return，每条都
 * 顺手 aspace_free 一次就等于"总有一天会漏一条"——漏掉的那一趟会让池子
 * 少一格，跑够 TASK_SLOTS 次之后所有 exec 都返回 -3，而且看不出是谁漏的。
 *
 * 成功时不还：对象连同页一起归任务，由 exec_task_dtor 在任务死后 free。
 * 于是"谁负责 free"只有两个可能，且互相排斥。 */
static int exec_run_job(struct exec_job *j, const char *name, int argc,
                        const char *const *argv,
                        unsigned long *out_magic, unsigned long *out_value)
{
    struct aspace *as;
    int rc;

    as = aspace_new();
    if (!as) {
        n_fail++;
        return -3;
    }
    j->as = as;

    rc = exec_stage(j, name, argc, argv, out_magic, out_value);
    if (rc != 0) {
        aspace_free(as);
        j->as = 0;
    }
    return rc;
}

/* 对外入口：占一格 job → 跑真身 → **无论成败都放手**。
 * busy 的清理只写在这一处，是因为真身里有十几条失败 return；
 * 每条都记得清一次就等于"总有一天会漏一条"。
 * 占不到格子（TASK_SLOTS 格全在用）返回 -7：明确失败，不排队。 */
int exec_run(const char *name, int argc, const char *const *argv,
             unsigned long *out_magic, unsigned long *out_value)
{
    struct exec_job *j = 0;
    int ji, rc;

    for (ji = 0; ji < TASK_SLOTS; ji++) {
        if (!jobs[ji].busy) {
            j = &jobs[ji];
            break;
        }
    }
    if (!j) {
        n_fail++;
        return -7;
    }
    last_job = ji;
    j->busy = 1;
    rc = exec_run_job(j, name, argc, argv, out_magic, out_value);
    j->busy = 0;
    return rc;
}

/* 现在有几格 job 正被占着（"没有残留"的可数证据；跑完应当是 0） */
int exec_slots_used(void)
{
    int n = 0, i;

    for (i = 0; i < TASK_SLOTS; i++) {
        if (jobs[i].busy)
            n++;
    }
    return n;
}

unsigned int exec_count(void)       { return n_exec; }
unsigned int exec_failures(void)    { return n_fail; }
/* 走 ELF 路径的次数（0 说明一直在跑裸二进制）。这是"格式分派真的发生"
 * 的可数证据：cmd_exec 的 `e=` 就是它。 */
unsigned int exec_elf_count(void)   { return elf_loaded; }
unsigned int exec_last_pages(void)  { return last_pages; }
unsigned int exec_last_handed(void) { return last_handed; }
/* 代码页 + 栈页：页在任务活着的时候就归它了（见 exec_run 的 task_user_offer） */
unsigned int exec_last_user_pages(void)
{
    return last_pages ? last_pages + 1 : 0;
}
/* 证据接口读"最近一趟那一格"。还没跑过（last_job<0）时一律给 0 /
 * TASK_DEATH_EXIT，不假装跑过。用指针取而不是 memcpy 一份，是因为这些值
 * 在 spawn 之后还在被 dtor 改（done 就是它置的）。 */
static struct exec_job *last_job_ptr(void)
{
    return (last_job >= 0 && last_job < TASK_SLOTS) ? &jobs[last_job] : 0;
}

unsigned long exec_exit_code(void)
{
    struct exec_job *j = last_job_ptr();
    return j ? j->exit : SYS_EXIT_NONE;
}
int exec_last_slot(void)
{
    struct exec_job *j = last_job_ptr();
    return j ? j->slot : 0;
}
int exec_last_death(void)
{
    struct exec_job *j = last_job_ptr();
    return j ? j->death : TASK_DEATH_EXIT;
}
/* 这一趟从页哪个偏移开始跑：裸二进制恒为 0；ELF 是 e_entry（ld 出来的通常
 * 就在文件头后面，几十字节），所以它是"ELF 分派真的生效"的第二个证据。 */
unsigned int exec_last_entry(void)
{
    struct exec_job *j = last_job_ptr();
    return j ? j->entry_rel : 0;
}
/* 池里此刻还占着几格。跑完必须回到 0——和 `jb=` 是一对：
 * `jb` 数 job 表，`asu` 数地址空间池，两个都归 0 才叫"这一趟没漏东西"。 */
unsigned int exec_live_aspaces(void)
{
    return aspace_used();
}
