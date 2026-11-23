/* tools/uprog2.ld —— 把 uprog2 拼成"跨两页"的小 ELF（p_memsz 越过 4KB）
 *
 * 与 uprog.ld 的差别只有一条：.bss 不给它排在 .text 后面，而是硬摆到 0x1800。
 * 于是 p_memsz ≥ 0x1804，exec 的 elf_span 算出 0x1804 -> 两页，
 * 走的是 page_alloc_run(2) 那条路，而不是往常的一页。
 *
 * 仍然是"文件偏移 == 虚拟地址"：一个 PT_LOAD、从文件第 0 字节起，
 * 中间那段 0x1800 之前的空洞由 ld 用文件里的 0 填出来（filesz 会跟着涨），
 * 所以原地搬家对 ELF 装载器依然安全。
 */
ENTRY(uprog2)

PHDRS
{
  text PT_LOAD FILEHDR PHDRS;
}

SECTIONS
{
  . = SIZEOF_HEADERS;
  .text : { *(.text) *(.text.*) } :text
  /* .bss 摆到第二页：memsz 因此越过 4096，这正是"跨页"的来源 */
  . = 0x1800;
  .bss : { *(.bss) *(.bss.*) } :text
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
