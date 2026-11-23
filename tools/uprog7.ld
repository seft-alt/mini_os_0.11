/* tools/uprog7.ld —— 只读代码段的 ELF：用来验证"用户页写保护"
 *
 * 与 tools/uprog.ld 唯一的区别是 PHDRS 里那一句 `FLAGS(5)`：
 *   PF_R (4) | PF_X (1) = 5，**没有 PF_W (2)**。
 * fs/elf.c 的 elf_load_map 按这条算权限图：这一页没被任何可写段覆盖，
 * 整段标 0 → exec 放行时清掉 PTE 的 RW 位 → ring3 写它就是 #PF。
 *
 * 其余安排（FILEHDR PHDRS、. = SIZEOF_HEADERS、DISCARD）与 uprog.ld 一致：
 * 文件偏移 == 虚拟地址，一段装进一页，满足 fs/elf.c 的"原地搬迁"前提。
 */
ENTRY(uprog)

PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
}

SECTIONS
{
  . = SIZEOF_HEADERS;
  .text : { *(.text) *(.text.*) } :text
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
