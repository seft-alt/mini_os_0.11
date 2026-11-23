/* fs/mosfs.c —— 对上层的五个动作（建/写/读/删文件、建/删目录）
 *
 * 拆分之后这个文件只剩"编排"：解析父路径 → 找/建 inode → 动目录项。
 * 真正干活的东西按 Linux 0.11 的分法放在旁边四个文件里：
 *
 *   fs/super.c    挂载、超级块、根缓存、计数
 *   fs/blkio.c    块工作缓冲、u16/u32、直接块/间接块解析
 *   fs/bitmap.c   数据块位图
 *   fs/inode.c    inode 读写分配 + free_inode（还块 + 标空）
 *   fs/namei.c    目录项比对、路径解析、目录增删（dir_add/dir_del/root_del）
 *
 * 对外仍然只有 include/fs/mosfs.h 这一份契约，调用方（shell / fd / exec）
 * 感觉不到拆分。
 * ====================================================== */
#include "linux/fsint.h"

/* 建目录：父目录必须在、末段建新目录。rc=0 建好、rc=1 已存在、
 * rc=-1 父不存在、rc=-2 父不是目录/目录满/不合法、rc=-3 读写失败、rc=-5 inode 用完。
 * 注意这里仍保留自己的一套切段（父 64 字节缓冲），与 path_split 的规则一致，
 * 只是它还要知道"父为空时算根"——两者行为必须有回归兜着（vfs.sh 三、四段）。 */
int fs_mkdir(const char *path)
{
    char parent[64];
    char name[FS_NAME_MAX + 1];
    unsigned int plen = 0, n = 0, i, slash = 0xFFFFFFFFu;
    unsigned int pino, pmode, psize, pblocks[FS_INODE_BLOCKS];
    unsigned int pblk, nino, nblk, zero[FS_INODE_BLOCKS] = { 0, 0, 0, 0, 0 };
    unsigned int k, off;

    if (!sb.ok || !path || path[0] == '\0')
        return -2;

    /* 切出末段名字与父路径 */
    for (i = 0; path[i] != '\0'; i++)
        if (path[i] == '/')
            slash = i;
    i = (slash == 0xFFFFFFFFu) ? 0 : slash + 1;
    while (path[i] != '\0' && path[i] != '/') {
        if (n < FS_NAME_MAX)
            name[n++] = path[i];
        i++;
    }
    name[n] = '\0';
    if (n == 0)
        return -2;                          /* 末段是空的 */

    if (slash == 0xFFFFFFFFu) {
        pino = sb.root;
    } else {
        for (i = 0; i < slash && i < sizeof(parent) - 1; i++)
            parent[plen++] = path[i];
        parent[plen] = '\0';
        if (plen == 0)
            pino = sb.root;                 /* "/d1" 的父就是根 */
        else {
            int r = fs_path_lookup(parent, &pino, &pmode, &psize);

            if (r == -2)
                return -2;
            if (r != 0)
                return -1;
        }
    }

    if (inode_read(pino, &pmode, &psize, pblocks) != 0 || pmode != 2)
        return -2;
    if (dir_find(pino, name, &nino, &pmode, &psize) > 0)
        return 1;                           /* 同名已经在 */
    if (psize + 16 > FS_BLOCK)
        return -2;                          /* v1 目录只占一块，满了就不再扩 */
    pblk = pblocks[0];
    if (pblk == 0)
        return -2;

    nino = inode_alloc();
    if (nino == 0)
        return -5;
    nblk = bitmap_alloc();
    if (nblk == 0)
        return -3;
    if (blk_read(nblk) != 0)
        return -3;
    for (k = 0; k < FS_BLOCK; k++)
        blk.b[k] = 0;
    if (blk_write(nblk) != 0)
        return -3;
    zero[0] = nblk;
    if (inode_write(nino, 2, 0, zero) != 0)
        return -3;

    /* 父目录尾部追加一项（先数据块、再 inode，与写路径同序） */
    if (blk_read(pblk) != 0)
        return -3;
    off = psize;
    for (k = 0; k < FS_NAME_MAX; k++)
        blk.b[off + k] = (k < n) ? (unsigned char)name[k] : 0;
    u32_put(off + 12, nino);
    if (blk_write(pblk) != 0)
        return -3;
    if (inode_write(pino, 2, psize + 16, pblocks) != 0)
        return -3;

    /* 内存里的根目录表跟着加一项，否则紧接着的 ls 看不到它 */
    if (pino == sb.root && ndent < FS_V1_MAX_FILES) {
        for (k = 0; k <= FS_NAME_MAX; k++)
            dent[ndent].name[k] = (k < n) ? name[k] : '\0';
        dent[ndent].ino = nino;
        dent[ndent].size = 0;
        ndent++;
    }
    return 0;
}

int fs_read(unsigned int ino, unsigned int offset, void *buf, unsigned int len)
{
    unsigned int mode, fsize, blocks[FS_INODE_BLOCKS];
    unsigned int done = 0;
    unsigned char *dst = (unsigned char *)buf;

    if (!sb.ok || !buf)
        return -1;
    if (inode_read(ino, &mode, &fsize, blocks) != 0)
        return -1;
    if (mode != 1)                             /* 目录不当文件读：调用方应该用 ls */
        return -1;

    while (done < len && offset + done < fsize) {
        unsigned int bi = (offset + done) / FS_BLOCK;
        unsigned int in_off = (offset + done) % FS_BLOCK;
        unsigned int n = FS_BLOCK - in_off;
        unsigned int left = fsize - (offset + done);
        unsigned int blkno, j;

        if (n > left)
            n = left;
        if (n > len - done)
            n = len - done;
        if (blk_lookup(blocks, bi, &blkno) != 0)
            break;                             /* 文件中间的洞：读到这儿为止 */
        if (blk_read(blkno) != 0)
            return -1;
        for (j = 0; j < n; j++)
            dst[done + j] = blk.b[in_off + j];
        done += n;
    }
    return (int)done;
}

/* 建文件：找 inode → 在父目录尾巴上加 16 字节 → 父 inode 的 size +16。
 * 失败码：-1 没挂载/父不存在 / -2 父不是目录或目录满 / -3 目录块坏了
 *         -4 名字太长或为空 / -5 inode 用完了 / -6 目录块写 / -7 父 inode 写 */
int fs_create(const char *name, unsigned int *out_ino)
{
    char parent[PATH_PARENT_MAX], leaf[FS_NAME_MAX + 1];
    unsigned int pino = sb.root, pmode = 2, psize = 0;
    unsigned int ino = 0, k, len = 0, rc;

    if (!sb.ok || !name || !out_ino)
        return -1;
    if (path_split(name, parent, leaf) != 0)
        return -4;

    while (leaf[len] != '\0')
        len++;

    if (parent[0] != '\0') {
        /* 多级：父必须真实存在且是目录，重名走 dir_find（不吃根缓存——
         * 根缓存里只有根目录那 8 项，子目录的项它没有） */
        if (fs_path_lookup(parent, &pino, &pmode, &psize) != 0)
            return -1;
        if (pmode != 2)
            return -2;
        if (dir_find(pino, leaf, &ino, &pmode, &psize) == 1) {
            *out_ino = ino;                     /* 已经有了：当追加用，不重建 */
            return 1;
        }
    } else {
        if (fs_lookup(leaf, &ino, &psize)) {
            *out_ino = ino;
            return 1;
        }
        if (ndent >= FS_V1_MAX_FILES)
            return -2;
    }

    ino = inode_alloc();
    if (!ino)
        return -5;
    rc = dir_add(pino, leaf, ino);
    if (rc != 0) {
        inode_zero(ino);                        /* 项没写进去就把号还回去，别留孤儿 */
        return rc;
    }

    /* 内存里的 dent[] 只有根目录那张，子目录不用同步 */
    if (pino == sb.root && ndent < FS_V1_MAX_FILES) {
        dent[ndent].ino = ino;
        dent[ndent].size = 0;
        for (k = 0; k <= FS_NAME_MAX; k++)
            dent[ndent].name[k] = (k < len) ? leaf[k] : '\0';
        ndent++;
    }
    n_creates++;
    *out_ino = ino;
    return 0;
}

/* 追加写：从文件现有尾部往后接。最多 FS_DIRECT*FS_BLOCK = 2048 字节。
 * 失败码：-1 没挂载/坏参 / -2 inode 坏了或不是文件 / -3 超过块上限
 *         -4 位图没块了 / -5 数据块读或写失败 / -6 inode 写失败 */
int fs_write(unsigned int ino, const void *buf, unsigned int len)
{
    unsigned int mode, size, blocks[FS_INODE_BLOCKS], j;
    unsigned int done = 0;
    const unsigned char *src = (const unsigned char *)buf;

    if (!sb.ok || !buf)
        return -1;
    if (len == 0)
        return 0;
    if (inode_read(ino, &mode, &size, blocks) != 0 || mode != 1)
        return -2;
    if (size + len > FS_MAX_BYTES)
        return -3;

    while (done < len) {
        unsigned int pos = size + done;
        unsigned int bi = pos / FS_BLOCK;
        unsigned int in_off = pos % FS_BLOCK;
        unsigned int n = FS_BLOCK - in_off;
        unsigned int blkno, rc;

        if (n > len - done)
            n = len - done;
        rc = blk_ensure(blocks, bi, &blkno);   /* 直接块或间接块，差一块就补一块 */
        if (rc == -4)
            return -4;
        if (rc != 0)
            return -3;
        if (blk_read(blkno) != 0)              /* 先读再写：块内已有的字节不能丢 */
            return -5;
        for (j = 0; j < n; j++)
            blk.b[in_off + j] = src[done + j];
        if (blk_write(blkno) != 0)
            return -5;
        done += n;
    }

    if (inode_write(ino, 1, size + len, blocks) != 0)
        return -6;
    for (j = 0; j < ndent; j++)
        if (dent[j].ino == ino)
            dent[j].size = size + len;          /* ls 显示的大小跟着走 */
    return (int)done;
}

/* 删文件：位图清位 → inode 归零 → 目录项抽掉（见 free_inode 的注释）。
 * 路径带 '/' 就走父目录解析，否则仍是根目录那一条老路。
 *
 * 失败码：-1 没挂载/找不到 / -2 是目录或名字与实际指向不符 /
 *         -3 inode 读不出来 / -4 位图读写失败 / -5 inode 写失败 /
 *         -6 目录块读写失败（v1 一块到底） / -7 父 inode 写失败 */
int fs_unlink(const char *name)
{
    char parent[PATH_PARENT_MAX], leaf[FS_NAME_MAX + 1];
    unsigned int pino = sb.root, ino = 0, idx = 0;
    int rc, root;

    if (!sb.ok || !name)
        return -1;
    if (path_split(name, parent, leaf) != 0)
        return -1;
    root = (parent[0] == '\0');

    rc = locate_in_dir(parent, leaf, &pino, &ino, &idx);
    if (rc != 0)
        return rc;

    rc = free_inode(ino);                       /* -2 目录 / -3 / -4 / -5 */
    if (rc != 0)
        return rc;

    if (root) {
        rc = root_del(idx);                     /* -6 / -7 */
        if (rc != 0)
            return rc;
    } else {
        rc = dir_del(pino, idx);                /* -6 / -7 */
        if (rc != 0)
            return rc;
    }

    n_unlinks++;
    return 0;
}

/* 删空目录：订正顺序与删文件一致——先还块（位图）、再把 inode 标空、
 * 最后才从父目录抽掉名字。非空就一个字节都不动，直接 -2：这样半途失败
 * 最坏是「目录还在、里面空了」，不会出现「名字没了、块永远占着」。
 *
 * 目录只有一块（v1 一块到底），所以只还 blocks[0]；真出现额外的块说明
 * 这张盘不是这套格式造的，宁可不删。
 * 失败码：-1 没挂载/找不到 / -2 不是目录、目录非空、或名字指向不符 /
 *         -3 inode 读不出来 / -4 位图读写失败 / -5 inode 写失败 /
 *         -6 目录块读写失败 / -7 父 inode 写失败 */
int fs_rmdir(const char *path)
{
    char parent[PATH_PARENT_MAX], leaf[FS_NAME_MAX + 1];
    unsigned int pino = sb.root, ino = 0, idx = 0, j, q;
    unsigned int mode, size, blocks[FS_INODE_BLOCKS];
    int rc, root;

    if (!sb.ok || !path)
        return -1;
    if (path_split(path, parent, leaf) != 0)
        return -1;
    root = (parent[0] == '\0');

    rc = locate_in_dir(parent, leaf, &pino, &ino, &idx);
    if (rc != 0)
        return rc;
    if (ino == sb.root)
        return -2;                              /* 根目录没有名字，但防一手 */

    if (inode_read(ino, &mode, &size, blocks) != 0)
        return -3;
    if (mode != 2)
        return -2;                              /* 普通文件走 rm，不走这 */
    if (size != 0)
        return -2;                              /* 非空：一个字节都不动 */
    for (j = 0; j < FS_INODE_BLOCKS; j++) {
        if (j == 0)
            continue;
        if (blocks[j])
            return -2;                          /* 目录多的块来路不明，不删 */
    }
    q = blocks[0];
    if (q && bitmap_clear(q) != 0)
        return -4;                              /* 先账 */
    if (inode_zero(ino) != 0)
        return -5;                              /* 再号 */

    if (root) {
        rc = root_del(idx);                     /* -6 / -7 */
        if (rc != 0)
            return rc;
    } else {
        rc = dir_del(pino, idx);
        if (rc != 0)
            return rc;
    }

    n_unlinks++;
    return 0;
}
