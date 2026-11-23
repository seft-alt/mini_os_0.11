/* fs/namei.c —— 目录语义与路径解析（对应 Linux 0.11 的 fs/namei.c）
 *
 * 这里回答三个问题：
 *   1. 一个名字在一个目录里对应哪个号（dir_find / fs_lookup / name_eq）
 *   2. 一条 "d1/d2/x" 的路径走到哪个 inode（fs_path_lookup / path_split）
 *   3. 往目录里加一条、从目录里抽一条要怎么动盘（dir_add / dir_del / root_del）
 *
 * 根目录比子目录多一份内存镜像（dent[]），所以"抽掉根目录第 idx 条"单独写成
 * root_del——它必须让盘上和内存一起变，子目录则只需要 dir_del。
 * ====================================================== */
#include "linux/fsint.h"

/* mount 时把根目录整张表读进 dent[]：名字、inode 号、大小。
 * 这是"一次读、之后零读"的做法——`ls` 与 `cat` 的查找都变成内存比较。 */
int root_load(void)
{
    unsigned int mode, dsize, blocks[FS_INODE_BLOCKS];
    unsigned int per_blk = FS_BLOCK / 16;
    unsigned int i, n;

    ndent = 0;
    if (inode_read(sb.root, &mode, &dsize, blocks) != 0 || mode != 2)
        return -1;

    n = dsize / 16;
    if (n > FS_V1_MAX_FILES)
        n = FS_V1_MAX_FILES;

    for (i = 0; i < n; i++) {
        unsigned int bi = i / per_blk;
        unsigned int off = (i % per_blk) * 16;
        unsigned int k, eino, m2, s2, b2[FS_INODE_BLOCKS];

        if (bi >= FS_DIRECT || blocks[bi] == 0)
            break;
        if (blk_read(blocks[bi]) != 0)
            return -1;

        for (k = 0; k < FS_NAME_MAX; k++) {
            dent[i].name[k] = (char)blk.b[off + k];
            if (dent[i].name[k] == '\0')
                break;
        }
        dent[i].name[FS_NAME_MAX] = '\0';

        /* 目录项只是一个"名字 → 号"的表，size 得回 inode 取 */
        eino = u32_at(off + 12);
        dent[i].ino = eino;
        dent[i].size = 0;
        if (eino < sb.inodes && inode_read(eino, &m2, &s2, b2) == 0) {
            if (m2 == 1)
                dent[i].size = s2;
            else if (m2 == 2)
                dent[i].size = s2 / 16;     /* 目录印项数，见 fs_dir_entry_at */
        }
    }
    ndent = i;
    return 0;
}

unsigned int fs_dir_count(void)
{
    return ndent;
}

int fs_dir_entry(unsigned int idx, char *name, unsigned int *ino, unsigned int *size)
{
    unsigned int k;

    if (idx >= ndent)
        return 0;
    for (k = 0; k <= FS_NAME_MAX; k++)
        name[k] = dent[idx].name[k];
    *ino = dent[idx].ino;
    *size = dent[idx].size;
    return 1;
}

int fs_lookup(const char *name, unsigned int *ino, unsigned int *size)
{
    unsigned int i, k;

    if (!name)
        return 0;
    for (i = 0; i < ndent; i++) {
        for (k = 0; k <= FS_NAME_MAX; k++) {
            if (dent[i].name[k] != name[k])
                break;
            if (name[k] == '\0') {             /* 两边同时到结尾 = 命中 */
                *ino = dent[i].ino;
                *size = dent[i].size;
                return 1;
            }
        }
    }
    return 0;
}

/* 目录项名与给定名字逐字节比：目录项 12 字节 NUL 补齐，名字也可能很短。 */
int name_eq(const unsigned char *ent, const char *nm)
{
    unsigned int k;

    for (k = 0; k < FS_NAME_MAX; k++) {
        if (ent[k] != (unsigned char)nm[k])
            return 0;
        if (ent[k] == '\0')
            return 1;                       /* 两边同时到结尾 = 命中 */
    }
    return nm[FS_NAME_MAX] == '\0';
}

/* 在目录 dir_ino 里找一项。返回 1 = 命中、0 = 没有、-1 = 读块失败。
 * 目录项只存"名字 → 号"，mode/size 得回 inode 取。 */
int dir_find(unsigned int dir_ino, const char *nm,
             unsigned int *out_ino, unsigned int *out_mode,
             unsigned int *out_size)
{
    unsigned int dmode, dsize, blocks[FS_INODE_BLOCKS];
    unsigned int per_blk = FS_BLOCK / 16;
    unsigned int n, i;

    if (inode_read(dir_ino, &dmode, &dsize, blocks) != 0 || dmode != 2)
        return 0;
    n = dsize / 16;
    for (i = 0; i < n; i++) {
        unsigned int bi = i / per_blk;
        unsigned int off = (i % per_blk) * 16;
        unsigned int blkno, eino, m2, s2, b2[FS_INODE_BLOCKS];

        if (blk_lookup(blocks, bi, &blkno) != 0)
            continue;                       /* 空洞：这一段没有数据块 */
        if (blk_read(blkno) != 0)
            return -1;
        if (!name_eq(&blk.b[off], nm))
            continue;
        eino = u32_at(off + 12);
        if (eino == 0 || eino >= sb.inodes)
            return 0;
        if (inode_read(eino, &m2, &s2, b2) != 0)
            return 0;
        *out_ino = eino;
        *out_mode = m2;
        *out_size = s2;
        return 1;
    }
    return 0;
}

/* 路径解析：单段走根缓存（零块读），多段逐级 dir_find。
 * 返回 0 = 命中、-1 = 不存在/格式非法、-2 = 中途拿文件当目录。 */
int fs_path_lookup(const char *path, unsigned int *out_ino,
                   unsigned int *out_mode, unsigned int *out_size)
{
    char seg[FS_NAME_MAX + 1];
    unsigned int cur, cmode, csize, i, n;

    if (!sb.ok || !path || path[0] == '\0')
        return -1;

    /* 不带 '/'：先走根目录内存缓存。这条路径零块读，
     * `cat hello.txt` 的读块数全靠它不变。 */
    for (i = 0; path[i] != '\0'; i++)
        if (path[i] == '/')
            break;
    if (path[i] == '\0') {
        unsigned int mode, size, blocks[FS_INODE_BLOCKS];

        if (!fs_lookup(path, &cur, &size))
            return -1;
        if (inode_read(cur, &mode, &size, blocks) != 0)
            return -1;
        *out_ino = cur;
        *out_mode = mode;
        *out_size = size;
        return 0;
    }

    cur = sb.root;
    cmode = 2;
    csize = 0;
    i = (path[0] == '/') ? 1u : 0u;
    while (1) {
        int r;

        for (n = 0; path[i] != '\0' && path[i] != '/'; i++)
            if (n < FS_NAME_MAX)
                seg[n++] = path[i];
        seg[n] = '\0';
        if (n == 0)
            return -1;                      /* "//" 或结尾多余的 '/' */
        if (cmode != 2)
            return -2;                      /* 拿文件当目录用 */
        r = dir_find(cur, seg, &cur, &cmode, &csize);
        if (r <= 0)
            return -1;
        if (path[i] == '\0')
            break;
        i++;                                /* 跳过这一段后的 '/' */
    }
    *out_ino = cur;
    *out_mode = cmode;
    *out_size = csize;
    return 0;
}

unsigned int fs_dir_count_at(unsigned int dir_ino)
{
    unsigned int mode, size, blocks[FS_INODE_BLOCKS];

    if (!sb.ok)
        return 0;
    if (inode_read(dir_ino, &mode, &size, blocks) != 0 || mode != 2)
        return 0;
    return size / 16;
}

int fs_dir_entry_at(unsigned int dir_ino, unsigned int idx, char *name,
                    unsigned int *out_ino, unsigned int *out_mode,
                    unsigned int *out_size)
{
    unsigned int dmode, dsize, blocks[FS_INODE_BLOCKS];
    unsigned int per_blk = FS_BLOCK / 16;
    unsigned int bi, off, blkno, eino, m2, s2, b2[FS_INODE_BLOCKS], k;

    if (!sb.ok)
        return 0;
    if (inode_read(dir_ino, &dmode, &dsize, blocks) != 0 || dmode != 2)
        return 0;
    if (idx >= dsize / 16)
        return 0;
    bi = idx / per_blk;
    off = (idx % per_blk) * 16;
    if (blk_lookup(blocks, bi, &blkno) != 0 || blk_read(blkno) != 0)
        return 0;
    for (k = 0; k < FS_NAME_MAX; k++) {
        name[k] = (char)blk.b[off + k];
        if (name[k] == '\0')
            break;
    }
    name[FS_NAME_MAX] = '\0';
    eino = u32_at(off + 12);
    *out_ino = eino;
    *out_mode = 0;
    *out_size = 0;
    if (eino < sb.inodes && inode_read(eino, &m2, &s2, b2) == 0) {
        *out_mode = m2;
        /* 目录印"几项"而不是字节数：和 `ls <目录>` 的项数口径统一，
         * 否则同一个子目录在根表里是 16、在 ls 里是 1，看着像两个东西。 */
        *out_size = (m2 == 2) ? (s2 / 16) : s2;
    }
    return 1;
}

/* 把 "d1/d2/x" 切成父路径 "d1/d2" 和末段 "x"。父为空串表示根目录。
 * 规则与读路径一致：末段为空（"a/"）或超过 12 字节一律不认。 */
int path_split(const char *path, char *parent, char *leaf)
{
    unsigned int i, n, sl = 0, seen = 0;

    if (!path || path[0] == '\0')
        return -1;
    for (i = 0; path[i] != '\0'; i++)
        if (path[i] == '/') {
            sl = i;
            seen = 1;
        }
    if (!seen) {
        parent[0] = '\0';
        for (n = 0; path[n] != '\0'; n++) {
            if (n >= FS_NAME_MAX)
                return -1;
            leaf[n] = path[n];
        }
        leaf[n] = '\0';
        return 0;
    }
    if (sl == 0) {                              /* "/x" → 父是根 */
        parent[0] = '\0';
    } else {
        for (n = 0; n < sl; n++) {
            if (n >= PATH_PARENT_MAX - 1)
                return -1;
            parent[n] = path[n];
        }
        parent[n] = '\0';
    }
    for (n = 0; path[sl + 1 + n] != '\0'; n++) {
        if (n >= FS_NAME_MAX)
            return -1;
        leaf[n] = path[sl + 1 + n];
    }
    leaf[n] = '\0';
    return (n == 0) ? -1 : 0;                   /* 末段空 = "d1/" */
}

/* 在 parent_ino 的目录块尾部写一条 16 字节项，并把父 inode 的 size +16。
 * 只负责"名字 → 号"这一件事；inode 由调用方先分配、失败要自己回收。
 * 失败码：-2 父目录满（v1 一块到底）/-3 父不是目录/-6 块读写/-7 inode 写。 */
int dir_add(unsigned int parent_ino, const char *name, unsigned int ino)
{
    unsigned int pmode, psize, pblocks[FS_INODE_BLOCKS], off, k, len = 0;

    if (inode_read(parent_ino, &pmode, &psize, pblocks) != 0 || pmode != 2)
        return -3;
    if (pblocks[0] == 0 || psize + 16 > FS_BLOCK)
        return -2;
    if (blk_read(pblocks[0]) != 0)
        return -6;
    while (name[len] != '\0')
        len++;
    off = psize;
    for (k = 0; k < FS_NAME_MAX; k++)
        blk.b[off + k] = (k < len) ? (unsigned char)name[k] : 0;
    u32_put(off + 12, ino);
    if (blk_write(pblocks[0]) != 0)
        return -6;
    if (inode_write(parent_ino, 2, psize + 16, pblocks) != 0)
        return -7;
    return 0;
}

/* 抽掉 parent_ino 目录里的第 idx 条：后面的往前挪 16 字节，末尾清 0。
 * 失败码同上，只是这里没有"满"这种可能。 */
int dir_del(unsigned int parent_ino, unsigned int idx)
{
    unsigned int pmode, psize, pblocks[FS_INODE_BLOCKS], k, q;

    if (inode_read(parent_ino, &pmode, &psize, pblocks) != 0 || pmode != 2)
        return -6;
    if (pblocks[0] == 0 || psize > FS_BLOCK || idx >= psize / 16)
        return -6;
    if (blk_read(pblocks[0]) != 0)
        return -6;
    for (k = idx; (k + 1) * 16 < psize; k++)
        for (q = 0; q < 16; q++)
            blk.b[k * 16 + q] = blk.b[(k + 1) * 16 + q];
    for (q = 0; q < 16; q++)
        blk.b[(psize / 16 - 1) * 16 + q] = 0;
    if (blk_write(pblocks[0]) != 0)
        return -6;
    if (inode_write(parent_ino, 2, psize - 16, pblocks) != 0)
        return -7;
    return 0;
}

/* 在一张目录里找"号等于 ino"的那条的下标。只比目录项里的号，不读子 inode。 */
int dir_index_of(unsigned int parent_ino, unsigned int ino, unsigned int *out_idx)
{
    unsigned int pmode, psize, pblocks[FS_INODE_BLOCKS];
    unsigned int per = FS_BLOCK / 16, n, i;

    if (inode_read(parent_ino, &pmode, &psize, pblocks) != 0 || pmode != 2)
        return 0;
    n = psize / 16;
    for (i = 0; i < n; i++) {
        unsigned int bi = i / per, off = (i % per) * 16, blkno;

        if (blk_lookup(pblocks, bi, &blkno) != 0)
            continue;
        if (blk_read(blkno) != 0)
            return 0;
        if (u32_at(off + 12) == ino) {
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

/* 从根目录抽掉第 idx 项：盘上移位 + 根 inode size-16 + 内存 dent[] 跟着移。
 * 子目录走 dir_del，根目录不能——它多一份内存镜像，忘了移就会「盘上没了、
 * ls 还看得见」。
 * 失败码：-6 根目录块读写 / -7 根 inode 写。 */
int root_del(unsigned int idx)
{
    unsigned int rmode, rsize, rblocks[FS_INODE_BLOCKS], k, q;

    if (idx >= ndent)
        return -6;
    if (inode_read(sb.root, &rmode, &rsize, rblocks) != 0 || rmode != 2)
        return -6;
    if (rblocks[0] == 0 || rsize > FS_BLOCK)
        return -6;
    if (blk_read(rblocks[0]) != 0)
        return -6;
    for (k = idx; k + 1 < ndent; k++)
        for (q = 0; q < 16; q++)
            blk.b[k * 16 + q] = blk.b[(k + 1) * 16 + q];
    for (q = 0; q < 16; q++)
        blk.b[(ndent - 1) * 16 + q] = 0;
    if (blk_write(rblocks[0]) != 0)
        return -6;
    if (inode_write(sb.root, 2, rsize - 16, rblocks) != 0)
        return -7;
    for (k = idx; k + 1 < ndent; k++)
        dent[k] = dent[k + 1];
    ndent--;
    return 0;
}

/* 在父目录里定位 leaf 那一条：给出父号、子号、下标。删文件与删目录共用。
 * 父为空串 = 根：根目录的目录项就在内存 dent[] 里，不必再去读块。
 * 失败码：-1 父不存在或名字不存在 / -2 父不是目录、或名字在但下标对不上。 */
int locate_in_dir(const char *parent, const char *leaf,
                  unsigned int *out_pino, unsigned int *out_ino,
                  unsigned int *out_idx)
{
    if (parent[0] == '\0') {
        unsigned int ino = 0, size = 0, j;

        if (!fs_lookup(leaf, &ino, &size))
            return -1;
        for (j = 0; j < ndent; j++)
            if (dent[j].ino == ino) {
                *out_pino = sb.root;
                *out_ino = ino;
                *out_idx = j;
                return 0;
            }
        return -2;                              /* 盘上有名字、内存表里没这条 */
    } else {
        unsigned int pino = 0, pmode = 0, psize = 0, ino = 0, idx = 0;

        if (fs_path_lookup(parent, &pino, &pmode, &psize) != 0)
            return -1;
        if (pmode != 2)
            return -2;
        if (dir_find(pino, leaf, &ino, &pmode, &psize) != 1)
            return -1;
        if (dir_index_of(pino, ino, &idx) != 1)
            return -2;
        *out_pino = pino;
        *out_ino = ino;
        *out_idx = idx;
        return 0;
    }
}
