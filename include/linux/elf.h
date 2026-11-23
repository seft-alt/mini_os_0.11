/* ==============================================================
 * elf：只认"段能装进 exec 给的那一段连续页"的 ELF32 可执行文件
 *
 * 之前 exec 只认裸二进制：从文件的第 0 字节开始，位置无关、代码必须自己
 * 从页头开始跑。这条路能跑，但把"程序从哪儿开始执行"写死成了 0，
 * 也让编译器产物进不来（gcc/ld 出来的东西前面是一堆文件头和段表）。
 * 这一层只补一件事：**读 ELF 头，按 PT_LOAD 把段搬进页，返回 e_entry**。
 *
 * 刻意不做（都是明确的限制，不是遗漏）：
 *   - 重定位、动态链接、PT_DYNAMIC、符号表；
 *   - 就地拷贝以外的"文件偏移 ≠ 虚拟地址"的重叠段（缓冲区既当源又当目的时
 *     只有 offset == vaddr 才是安全的）；
 *   - 段与段之间"倒序/重叠"的摆放（按程序头顺序逐段搬，假定各段在
 *     vaddr 上互不重叠——ld 产出的正常 ELF 就是这样）。
 *
 * 一个段可以跨页，但必须整段落在 exec 给的 dst_bytes 之内（页数 × 4KB）。
 *
 * 校验顺序与失败码：
 *   -1 不是 ELF32/LE/ET_EXEC/EM_386 或头部不自洽
 *   -2 程序头表越界
 *   -3 段落不进 dst_bytes（vaddr + memsz 越界）
 *   -4 段在文件里的范围越界或 filesz > memsz
 *   -5 原地拷贝但文件偏移与虚拟地址不一致
 *   -6 一个 PT_LOAD 都没有
 *   -7 入口地址不在 dst_bytes 之内
 * ====================================================== */
#ifndef MINI_OS_ELF_H
#define MINI_OS_ELF_H

/* 是不是一个我们认的 ELF32 可执行文件（只看头，不搬内容） */
int elf_probe(const unsigned char *img, unsigned int len);

/* 把各 PT_LOAD 段拷进 dst（dst 就是那一页的页头），*entry 得到 e_entry。
 * src 与 dst 允许是同一块缓冲（exec 就是"先 fs_read 进页、再原地搬"）。
 * dst_bytes 是**整段**能用的字节数（跨页时是页数 × 4KB），
 * 一个段仍然要整个落进 dst_bytes，但可以跨页。 */
int elf_load(const unsigned char *img, unsigned int len, unsigned char *dst,
             unsigned int dst_bytes, unsigned int *entry);

/* elf_load 的"带页权限图"版：wmap 是一段 wmap_n 字节的调用方数组（n = 页数），
 * 返回后 wmap[i] = 1 表示第 i 页被某个 PF_W（可写）段覆盖，0 = 只读。
 * 初始整段清 0（保守：没被可写段碰过的页一律当作只读），因此
 * "只有一个 RWX 段的 ld 产物"整段都是 1，"纯代码段（FLAGS(5)）"整段是 0。
 * wmap = NULL 时与 elf_load 完全等价（不产生权限图）。 */
int elf_load_map(const unsigned char *img, unsigned int len, unsigned char *dst,
                 unsigned int dst_bytes, unsigned int *entry,
                 unsigned char *wmap, unsigned int wmap_n);

/* 这段 ELF 需要多大的地址区间（所有 PT_LOAD 的 max(vaddr + memsz)）。
 * exec 用它决定"要几页"，返回 0 表示不是 ELF 或一个段都没有。 */
unsigned int elf_span(const unsigned char *img, unsigned int len);

#endif /* MINI_OS_ELF_H */
