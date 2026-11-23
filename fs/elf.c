/* fs/elf.c —— 极小的 ELF32 解析器（细节与失败码见 include/fs/elf.h）
 *
 * 只有两种读者：exec（装页用）与测试（判"这段字节是不是 ELF"）。
 * 所以这里不建结构体、不做字节序抽象——每个字段按 ELF32 小端的固定偏移读一次，
 * 偏移写死在这里比写一个 struct 更不容易被编译器对齐规则坑到。 */
#include "linux/elf.h"

#define EI_CLASS     4
#define EI_DATA      5
#define ET_EXEC      2
#define EM_386       3
#define PT_LOAD      1

#define EHDR_ENTRY   24     /* e_entry */
#define EHDR_PHOFF   28     /* e_phoff */
#define EHDR_PHNUM   44     /* e_phnum */
#define EHDR_PHENTSZ 42     /* e_phentsize */
#define EHDR_MACHINE 18     /* e_machine */
#define EHDR_TYPE    16     /* e_type */

#define PH_FILESZ    16     /* p_filesz */
#define PH_MEMSZ     20     /* p_memsz */
#define PH_OFFSET    4      /* p_offset */
#define PH_TYPE      0      /* p_type */
#define PH_VADDR     8      /* p_vaddr */
#define PH_FLAGS     24     /* p_flags */

#define PF_W         0x2U   /* 段可写 */
#define PAGE_SHIFT   12

/* 地址上界：恒等映射只覆盖到 16MB，超过这个就算坏 ELF（也顺手当溢出护栏用） */
#define ELF_VADDR_MAX 0x01000000U

static unsigned int r16(const unsigned char *p, unsigned int off)
{
    return (unsigned int)p[off] | ((unsigned int)p[off + 1] << 8);
}

static unsigned int r32(const unsigned char *p, unsigned int off)
{
    return (unsigned int)p[off] | ((unsigned int)p[off + 1] << 8) |
           ((unsigned int)p[off + 2] << 16) | ((unsigned int)p[off + 3] << 24);
}

int elf_probe(const unsigned char *img, unsigned int len)
{
    if (!img || len < 52)
        return 0;
    if (img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F')
        return 0;
    if (img[EI_CLASS] != 1               /* ELFCLASS32 */
        || img[EI_DATA] != 1)            /* ELFDATA2LSB */
        return 0;
    if (r16(img, EHDR_TYPE) != ET_EXEC || r16(img, EHDR_MACHINE) != EM_386)
        return 0;
    if (r32(img, EHDR_PHOFF) == 0 || r16(img, EHDR_PHNUM) == 0)
        return 0;
    if (r16(img, EHDR_PHENTSZ) != 32)
        return 0;
    return 1;
}

int elf_load(const unsigned char *img, unsigned int len, unsigned char *dst,
             unsigned int dst_bytes, unsigned int *entry)
{
    return elf_load_map(img, len, dst, dst_bytes, entry, 0, 0);
}

/* 装载本体。除了搬字节，还顺手产出"哪几页可写"的权限图：
 * 只有 p_flags 带 PF_W 的段覆盖到的页才算可写，其余按只读处理。
 * 这是保守方向——漏标一页可写会让程序在写自己的数据时被杀（现象明显、
 * 一次就能发现），多标一页可写则会让写保护形同虚设（现象隐蔽）。 */
int elf_load_map(const unsigned char *img, unsigned int len, unsigned char *dst,
                 unsigned int dst_bytes, unsigned int *entry,
                 unsigned char *wmap, unsigned int wmap_n)
{
    unsigned int phoff, phnum, i, loaded = 0;

    if (!elf_probe(img, len) || !dst || !entry)
        return -1;

    for (i = 0; i < wmap_n; i++)
        wmap[i] = 0;                         /* 默认只读：没被可写段碰过的页一律 RO */

    phoff = r32(img, EHDR_PHOFF);
    phnum = r16(img, EHDR_PHNUM);
    *entry = r32(img, EHDR_ENTRY);

    for (i = 0; i < phnum; i++) {
        unsigned int ph = phoff + i * 32;
        unsigned int foff, vaddr, filesz, memsz, k;

        if (ph + 32 > len)
            return -2;
        if (r32(img, ph + PH_TYPE) != PT_LOAD)
            continue;

        foff = r32(img, ph + PH_OFFSET);
        vaddr = r32(img, ph + PH_VADDR);
        filesz = r32(img, ph + PH_FILESZ);
        memsz = r32(img, ph + PH_MEMSZ);

        if (memsz == 0 && filesz == 0)
            continue;
        /* 减法给上界，避免 vaddr + memsz 溢出：vaddr > dst_bytes - memsz 即越界 */
        if (vaddr > dst_bytes || memsz > dst_bytes - vaddr)
            return -3;
        if (filesz > memsz)
            return -4;
        if (foff > len || filesz > len - foff)
            return -4;
        /* 原地搬（img == dst 的那一页）：只有 offset == vaddr 才是安全的，
         * 别的重叠组合需要一块临时缓冲，v1 不做 */
        if ((const unsigned char *)dst == img && foff != vaddr)
            return -5;

        for (k = 0; k < filesz; k++)
            dst[vaddr + k] = img[foff + k];
        for (k = filesz; k < memsz; k++)
            dst[vaddr + k] = 0;              /* .bss：文件里没有的部分必须清 0 */

        if (wmap && (r32(img, ph + PH_FLAGS) & PF_W) && memsz) {
            unsigned int p0 = vaddr >> PAGE_SHIFT;
            unsigned int p1 = (vaddr + memsz - 1) >> PAGE_SHIFT;

            for (k = p0; k <= p1 && k < wmap_n; k++)
                wmap[k] = 1;
        }
        loaded++;
    }

    if (loaded == 0)
        return -6;
    if (*entry >= dst_bytes)
        return -7;
    return 0;
}

/* 这段 ELF 要多大地址区间：所有 PT_LOAD 的 max(vaddr + memsz)。
 * 程序头表本身必须落在前 4KB（exec 只先读一页来判断要多大的段），
 * 所以 len 一般就是一页；越界按 0 处理，让调用方走"读不全"那条失败码。
 * 上界的算法和 elf_load 一样用减法，避免 vaddr + memsz 溢出。 */
unsigned int elf_span(const unsigned char *img, unsigned int len)
{
    unsigned int phoff, phnum, i, span = 0;

    if (!elf_probe(img, len))
        return 0;
    phoff = r32(img, EHDR_PHOFF);
    phnum = r16(img, EHDR_PHNUM);

    for (i = 0; i < phnum; i++) {
        unsigned int ph = phoff + i * 32;
        unsigned int vaddr, filesz, memsz, end;

        if (ph + 32 > len)
            return 0;
        if (r32(img, ph + PH_TYPE) != PT_LOAD)
            continue;
        vaddr = r32(img, ph + PH_VADDR);
        filesz = r32(img, ph + PH_FILESZ);
        memsz = r32(img, ph + PH_MEMSZ);
        if (memsz < filesz)
            memsz = filesz;             /* filesz > memsz 是坏 ELF，但先按大的算 */
        /* 只认用户态能用的地址：跑到 16MB 之外就别替它要页了 */
        if (vaddr >= ELF_VADDR_MAX || memsz > ELF_VADDR_MAX - vaddr)
            return 0;
        end = vaddr + memsz;
        if (end > span)
            span = end;
    }
    return span;
}
