/* ==============================================================
 * mosfs v1：一个"够用就好"的只读文件系统
 *
 * 为什么自造而不是直接读 FAT12：FAT12 的每一条规则（BPB 的 20 多个字段、
 * 簇大小、FAT 链、根目录定长区、0xFF8 以后的坏簇/尾簇语义）都要实现完才能读一个
 * 文件，而这一层现在的目的只是"让盘上的东西有名字"。mosfs 的全部规则在下面 40 行里，
 * 出错时能一眼算出该看哪个扇区。
 *
 * 磁盘布局（LBA=0 留给"扇区 0"，文件系统从 LBA 1 开始）：
 *   LBA 1           超级块（本文件 fs_mount 解析的偏移全写在下表）
 *   LBA 2 .. 3      inode 表（每个 32 字节，一块 16 个）
 *   LBA 4           位图（bit i = 块号 i 被占用；写路径从这里要块）
 *   LBA 5 ..        数据区
 *
 * 超级块偏移（u32 小端，单位都是 512 字节的块）：
 *   0  magic "MFS1"   4 block_size=512   8 total_blocks   12 inode_count
 *   16 root_ino        20 inode_start     24 inode_per_block
 *   28 bitmap_start    32 data_start      36 data_blocks
 *
 * inode（32 字节）：0 mode(u16, 1=文件 2=目录)  2 links(u16)  4 size(u32)
 *                   8 blocks[5]：4 个直接块 + 1 个间接块（间接块里是 128 个块号）
 *                   所以单文件上限 132 块 = 67584 字节（66KB）
 *
 * 目录项（16 字节）：0..11 name（12 字节，NUL 补齐）  12 ino(u32)
 *
 * 刻意不做的：二级间接、多级目录、时间戳、权限位。
 * v1 的写只有追加（fs_create / fs_write）；能删（fs_unlink）但删完的块不并回
 * 已有的文件，也没有截断——这些都要动"块号表"，留给下一个版本。
 * ============================================================== */
#ifndef MINI_OS_MOSFS_H
#define MINI_OS_MOSFS_H

#define FS_BLOCK        512
#define FS_MAGIC_LEN    4
#define FS_NAME_MAX     12
#define FS_DIRECT       4       /* inode 里的 4 个直接块 */
#define FS_INDIRECT     4       /* inode 里第 5 个块号 = 间接块（一块存 128 个块号） */
#define FS_INODE_BLOCKS 5       /* blocks[5]，32 字节的 inode 正好装得下（8+20=28） */
#define FS_IND_PER_BLK  (FS_BLOCK / 4)                       /* 128 */
#define FS_MAX_BLOCKS   (FS_DIRECT + FS_IND_PER_BLK)         /* 132 */
#define FS_MAX_BYTES    (FS_MAX_BLOCKS * FS_BLOCK)           /* 67584 = 66KB */
#define FS_BLOCK_LBA    1       /* 超级块所在扇区 */
#define FS_V1_MAX_INO   64
#define FS_V1_MAX_FILES 8       /* 根目录最多列 8 项：用户页一次只放得下 8 行 */

/* 挂载：读超级块、校验 magic 与块大小。0 = 成功，<0 = 失败码（负值） */
int fs_mount(void);

/* 挂载后的元数据（mount 失败时全为 0） */
unsigned int fs_blocks(void);
unsigned int fs_inodes(void);
unsigned int fs_reads(void);            /* 真落盘的块数（bcache 未命中） */
unsigned int fs_hits(void);             /* 块缓存命中次数（与 fs_reads 成对看） */
unsigned int fs_errors(void);           /* 挂载/读块失败次数 */

/* 根目录：项数 + 按下标取一项（name 输出缓冲区至少 FS_NAME_MAX+1 字节） */
unsigned int fs_dir_count(void);
int fs_dir_entry(unsigned int idx, char *name, unsigned int *ino, unsigned int *size);

/* 按名字在根目录里找一个文件；找到返回 1，并把 ino/size 写回 */
int fs_lookup(const char *name, unsigned int *ino, unsigned int *size);

/* ---------------------------------------------------------------
 * 路径（v1.1：多级目录）
 * ---------------------------------------------------------------
 * 路径规则只有三条：以 '/' 开头就是绝对路径；不带 '/' 的名字等价于
 * 根目录下那一个；段与段之间只认单斜杠，其它一律当"不存在"。
 * 单段走根目录内存缓存（`ls`/`cat hello.txt` 的块读因此一个不少）；
 * 多段逐级下盘，每一段的父必须是目录（mode == 2）。
 *
 * 返回：0 = 命中（补 ino/mode/size）、-1 = 找不到、-2 = 中间段不是目录。
 */
int fs_path_lookup(const char *path, unsigned int *ino,
                   unsigned int *mode, unsigned int *size);

/* 任意目录的项数 / 按下标取一项（给 `ls <路径>` 用）。
 * 目录不存在或不是目录时，项数为 0、取项返回 0。 */
unsigned int fs_dir_count_at(unsigned int dir_ino);
int fs_dir_entry_at(unsigned int dir_ino, unsigned int idx, char *name,
                    unsigned int *ino, unsigned int *mode, unsigned int *size);

/* 建目录：父路径必须已存在且是目录，末段是新名字。
 * 返回 0 = 建好、1 = 同名已存在、-1 = 父不存在、-2 = 父不是目录 /
 * 父目录已满一块 / 不合法、-3 = 块或 inode 读写失败、-5 = inode 用完。 */
int fs_mkdir(const char *path);

/* 删空目录（rmdir 的落点）：父路径照常解析，目标必须 mode==2 且 size==0，
 * 顺序是「还块 → inode 标空 → 从父目录抽项」，非空就一个字节都不动。
 * 返回 0 = 删掉、-1 = 父不存在/名字不存在、-2 = 不是目录 / 目录非空、
 * -3 = inode 读失败、-4 = 位图、-5 = inode 写、-6 = 目录块、-7 = 父 inode 写。 */
int fs_rmdir(const char *path);

/* 读文件的一段到 buf；返回实际读到的字节数，<0 表示出错 */
int fs_read(unsigned int ino, unsigned int offset, void *buf, unsigned int len);

/* ---------------------------------------------------------------
 * v1 的写路径（追加式，不做删除/覆盖）
 * ---------------------------------------------------------------
 * 顺序刻意定成"数据块 → inode → 位图"，和"位图先记上"相反：
 * 位图是**谁在用哪一块**的账，账先记上、数据没落盘，掉电后就是"这块被占了、
 * 但内容还是空的"——那种空洞比"这块还没记上"更难查。当前不做一致性恢复，
 * 所以宁可留下"能用但没登记"的块，也不要留"登记了却空的"块。
 *
 * fs_create：找空闲 inode（mode==0）、在根目录尾巴上加一项 16 字节目录项、
 *            把根目录 inode 的 size 加 16。同名文件已经存在时返回 1（不重复建）。
 *            返回 0 = 建好（*out_ino 是新号）、1 = 已存在（*out_ino 是旧号）、<0 = 失败。
 * fs_write： 从文件尾部追加 len 字节（最多 FS_DIRECT*FS_BLOCK = 2KB）。
 *            中间块是新的就往位图里要一块；返回写进去的字节数，<0 = 失败。
 * fs_writes / fs_creates / fs_unlinks：落盘的块数、建过的文件数、删过的文件数。
 * --------------------------------------------------------------- */
int fs_create(const char *name, unsigned int *out_ino);
int fs_write(unsigned int ino, const void *buf, unsigned int len);
unsigned int fs_writes(void);
unsigned int fs_creates(void);

/* 删文件（按名字）：位图清位 → inode 归零 → 目录项抽掉，顺序见 fs/mosfs.c。
 * 返回 0 = 删掉、<0 = 失败（-1 找不到、-2 是目录…见实现处）。 */
int fs_unlink(const char *name);
unsigned int fs_unlinks(void);

#endif /* MINI_OS_MOSFS_H */
