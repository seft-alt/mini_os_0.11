/* tools/uprog.ld —— 把 uprog 拼成"文件偏移 == 虚拟地址、一段装进一页"的小 ELF
 *
 * 默认的 ld 会把 .text 放到文件偏移 0x1000（按页对齐），结果是一个 4KB 多的
 * ELF，可 exec 只给一页。这里用 FILEHDR PHDRS 让第一个 PT_LOAD 从文件第 0 字节
 * 开始（头也在里面），于是 p_offset == p_vaddr == 0，e_entry 就落在几十字节处。
 * 这也正好满足 fs/elf.c 那条"原地搬迁只有 offset == vaddr 才安全"的限制。
 */
ENTRY(uprog)

PHDRS
{
  text PT_LOAD FILEHDR PHDRS;
}

SECTIONS
{
  /* 代码从"文件头之后"开始：既给 ELF 头和段表留出位置（否则 ld 报
   * "not enough room for program headers"），又让 p_offset/p_vaddr 都是 0 */
  . = SIZEOF_HEADERS;
  .text : { *(.text) *(.text.*) } :text
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
