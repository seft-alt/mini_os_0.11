/* ==============================================================
 * include/fs/fsint.h —— mosfs 拆文件之后的"内部共享头"
 *
 * `fs/mosfs.c` 原来是 1000 多行的一整块（超级块 + inode + 位图 + 目录 +
 * 路径解析 + 读写删全在一个文件里）。按 Linux 0.11 的语义切成五块之后，
 * 这些符号从 static 变成外部链接，只能借这个头互相看见：
 *
 *   fs/super.c   sb / fs_dev / 计数 / fs_mount / fs_probe
 *   fs/blkio.c   工作缓冲 blk + 字节序 + 块号解析
 *   fs/bitmap.c  块位图
 *   fs/inode.c   inode 读写分配 + free_inode
 *   fs/namei.c   目录项比对 / 路径解析 / 目录增删
 *   fs/mosfs.c   对外 API 的编排（create/write/read/unlink/rmdir/mkdir）
 *
 * 只有 fs/ 下的 .c 引这个头；外部（shell/fd/exec）只认 fs/mosfs.h。
 * ============================================================== */
#ifndef MINI_OS_FS_FSINT_H
#define MINI_OS_FS_FSINT_H

#include "linux/mosfs.h"

/* 父路径的栈缓冲上限。比 FD_PATH_MAX（40）宽一档，够两级目录再留余量。 */
#define PATH_PARENT_MAX 64

/* 工作缓冲：一次装一块盘（512 字节），所有"读进来改一点再写回去"都走它 */
union fs_block {
    unsigned short w[256];
    unsigned char b[FS_BLOCK];
};

/* 超级块读出来之后的状态（字段按字节摆，见 fs/super.c） */
struct fs_super {
    int ok;
    unsigned int block_size;
    unsigned int total;
    unsigned int inodes;
    unsigned int root;
    unsigned int inode_start;
    unsigned int inode_per_blk;
    unsigned int data_start;
    unsigned int data_blocks;
    unsigned int bitmap_start;
};

/* 根目录缓存的一行：名字 / 号 / 大小（目录存项数，文件存字节） */
struct fs_dent {
    char name[FS_NAME_MAX + 1];
    unsigned int ino;
    unsigned int size;
};

/* ---- fs/super.c ---- */
extern struct fs_super sb;
extern unsigned int fs_dev;                 /* 当前挂载的设备号（整盘或分区） */
extern unsigned int n_errors;
extern unsigned int n_writes, n_creates, n_unlinks;
extern struct fs_dent dent[FS_V1_MAX_FILES];
extern unsigned int ndent;

/* ---- fs/blkio.c ---- */
extern union fs_block blk;
unsigned int u32_at(unsigned int off);
unsigned int u16_at(unsigned int off);
void u16_put(unsigned int off, unsigned int v);
void u32_put(unsigned int off, unsigned int v);
int blk_read(unsigned int lba);
int blk_write(unsigned int lba);
int blk_lookup(const unsigned int *blocks, unsigned int bi, unsigned int *out);
int blk_ensure(unsigned int *blocks, unsigned int bi, unsigned int *out);

/* ---- fs/bitmap.c ---- */
unsigned int bitmap_alloc(void);
int bitmap_clear(unsigned int b);

/* ---- fs/inode.c ---- */
int inode_read(unsigned int ino, unsigned int *mode, unsigned int *size,
               unsigned int *blocks);
int inode_write(unsigned int ino, unsigned int mode, unsigned int size,
                const unsigned int *blocks);
unsigned int inode_alloc(void);
int inode_zero(unsigned int ino);
int free_inode(unsigned int ino);

/* ---- fs/namei.c ---- */
int root_load(void);
int name_eq(const unsigned char *ent, const char *nm);
int dir_find(unsigned int dir_ino, const char *nm, unsigned int *out_ino,
             unsigned int *out_mode, unsigned int *out_size);
int path_split(const char *path, char *parent, char *leaf);
int dir_add(unsigned int parent_ino, const char *name, unsigned int ino);
int dir_del(unsigned int parent_ino, unsigned int idx);
int dir_index_of(unsigned int parent_ino, unsigned int ino, unsigned int *out_idx);
int locate_in_dir(const char *parent, const char *leaf, unsigned int *out_pino,
                  unsigned int *out_ino, unsigned int *out_idx);
int root_del(unsigned int idx);

#endif /* MINI_OS_FS_FSINT_H */
