/* ==============================================================
 * 地址空间对象：一次 exec 造出来的那套页，打包成一个东西
 *
 * 以前这五样散在 `struct exec_job` 里：code / pages / stack / entry / esp0。
 * 散着放的问题不在“能不能跑”，而在“交接的时候要逐个字段记”：
 *   - 页给了任务，job 里的 code 还留着，看着像还归 exec；
 *   - dtor 里逐个清 4 个字段，漏一个就是悬空指针；
 *   - 要“发起后不等”时，调用方得记住 5 个数字才能去 reap。
 * 这一层把它们收成一个对象：谁造出来的 aspace，谁负责 free；交出去的时候
 * 只交一个指针。
 *
 * 池子是**静态**的（TASK_SLOTS 格），理由和 job 表一样：内核堆还没有
 * “可回收对象”这一类东西，而这类对象的最大数量本来就由槽位数封顶。
 * 用满就明确失败（aspace_new 返回 0），不排队、不静默覆盖。
 *
 * 生命周期约定（谁管参数）：
 *   aspace_new()  → 调用方拥有；
 *   exec 成功     → 所有权转移给任务（由 dtor 在任务死后 free）；
 *   exec 失败     → 调用方自己 free（exec_run_job 里那唯一一处）。
 * ============================================================== */
#ifndef MINI_OS_ASPACE_H
#define MINI_OS_ASPACE_H

#include "linux/task.h"          /* TASK_SLOTS */

struct aspace {
    unsigned int code;           /* 代码段第一页（连续 pages 页，恒等映射） */
    unsigned int pages;          /* 代码段占几页（1..EXEC_MAX_PAGES） */
    unsigned int stack;          /* 用户栈页 */
    unsigned int entry;          /* 从哪儿开始跑（绝对地址：裸二进制=页头，ELF=页头+e_entry） */
    unsigned long esp0;          /* 用户栈顶（= stack + 一页） */
    int slot;                    /* 这套页交给哪个槽位（0 = 还没交出去） */
    int live;                    /* 池里这一格被占着 */
};

/* 取一格空对象（字段全清零）。池满返回 0——明确失败，不排队。 */
struct aspace *aspace_new(void);

/* 还回去（幂等：对空指针、对已经归还过的对象都不做事）。
 * 只清账，**不动页**：页归任务，由调度器在任务死后收；失败路径的页由
 * exec 的 unwind 收。这里清字段是为了让“已经归还”这件事可判定。 */
void aspace_free(struct aspace *as);

/* 这一格在池里的下标（0..TASK_SLOTS-1），空指针返回 -1。
 * 排查"这个对象是哪一格"时用；运行逻辑不依赖它。 */
int aspace_index(const struct aspace *as);

/* 池里现在有几格被占着（跑完应当回到 0——这是“对象没漏”的可数证据）。 */
unsigned int aspace_used(void);

#endif /* MINI_OS_ASPACE_H */
