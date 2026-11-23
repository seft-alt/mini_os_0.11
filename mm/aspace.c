/* mm/aspace.c —— 地址空间对象池（契约见 include/mm/aspace.h）
 *
 * 只有几十行：取一格、清一格、数几格。刻意保持这么薄——对象的**语义**
 * （这些页归谁、什么时候收）写在 exec 与调度器那一侧，这里只负责
 * “是谁的、还活着没有”。
 */
#include "linux/aspace.h"

static struct aspace pool[TASK_SLOTS];

struct aspace *aspace_new(void)
{
    int i;

    for (i = 0; i < TASK_SLOTS; i++) {
        if (pool[i].live)
            continue;
        pool[i].code = 0;
        pool[i].pages = 0;
        pool[i].stack = 0;
        pool[i].entry = 0;
        pool[i].esp0 = 0;
        pool[i].slot = 0;
        pool[i].live = 1;
        return &pool[i];
    }
    return 0;
}

void aspace_free(struct aspace *as)
{
    if (!as || !as->live)
        return;
    as->code = 0;
    as->pages = 0;
    as->stack = 0;
    as->entry = 0;
    as->esp0 = 0;
    as->slot = 0;
    as->live = 0;
}

int aspace_index(const struct aspace *as)
{
    if (!as || !as->live)
        return -1;
    return (int)(as - pool);
}

unsigned int aspace_used(void)
{
    unsigned int n = 0;
    int i;

    for (i = 0; i < TASK_SLOTS; i++) {
        if (pool[i].live)
            n++;
    }
    return n;
}
