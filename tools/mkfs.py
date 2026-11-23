#!/usr/bin/env python3
"""tools/mkfs.py —— 在宿主机上造一张 mosfs 数据盘（给 QEMU 挂成 IDE 从盘）

用法：python3 tools/mkfs.py /tmp/fs.img
布局严格对齐 include/fs/mosfs.h 里写的偏移；改布局要同时改两处。
"""
import sys

BLOCK = 512
MAGIC = b"MFS1"

# 固定的 LBA 布局（和 mosfs.h 的表一致）
SB_LBA = 1
INODE_LBA = 2
INODE_PER_BLK = 16
INODE_COUNT = 32            # 32 个 inode = 2 块（LBA 2..3）
BITMAP_LBA = 4
DATA_LBA = 5


def u32(v):
    return v.to_bytes(4, "little")


def u16(v):
    return v.to_bytes(2, "little")


def inode(mode, size, blocks):
    b = bytearray(32)
    b[0:2] = u16(mode)
    b[2:4] = u16(1)                 # links
    b[4:8] = u32(size)
    for i, blk in enumerate(blocks[:4]):
        b[8 + i * 4:12 + i * 4] = u32(blk)
    return b


def dirent(name, ino):
    b = bytearray(16)
    n = name.encode()[:12]
    b[0:len(n)] = n
    b[12:16] = u32(ino)
    return b


def build(path, total_blocks=2048):
    data_blocks = total_blocks - DATA_LBA

    # 三个文件：一个短文本、一个跨块的长文本、一个纯二进制（验证不可打印字符的处理）
    hello = b"hello from mosfs v1\nsecond line\nthird line is short\n"
    long_text = ("A" * 78 + "\n").encode()            # 正好 78 列，不触发硬折
    wrapped = ("0123456789" * 9).encode()             # 90 字节单行 -> cat 硬折成两行
    blob = bytes([0x00, 0x01, 0x7F, 0x80, 0xFF]) * 4  # 不可打印，屏上应显示为 '.'
    files = [("hello.txt", hello), ("long.txt", long_text + wrapped),
             ("blob.bin", blob)]

    img = bytearray(total_blocks * BLOCK)

    # 超级块
    sb = bytearray(BLOCK)
    sb[0:4] = MAGIC
    sb[4:8] = u32(BLOCK)
    sb[8:12] = u32(total_blocks)
    sb[12:16] = u32(INODE_COUNT)
    sb[16:20] = u32(1)               # root ino
    sb[20:24] = u32(INODE_LBA)
    sb[24:28] = u32(INODE_PER_BLK)
    sb[28:32] = u32(BITMAP_LBA)
    sb[32:36] = u32(DATA_LBA)
    sb[36:40] = u32(data_blocks)
    img[SB_LBA * BLOCK:(SB_LBA + 1) * BLOCK] = sb

    # 数据块的分配：0 号给根目录，之后每个文件按需占用（最多 4 块 = 2KB）
    next_blk = DATA_LBA
    root_blk = next_blk
    next_blk += 1

    root_entries = bytearray()
    inodes = {}
    for i, (name, content) in enumerate(files, start=2):
        nblk = (len(content) + BLOCK - 1) // BLOCK
        assert nblk <= 4, "mosfs v1 只有 4 个直接块"
        blocks = []
        for k in range(nblk):
            blocks.append(next_blk)
            seg = content[k * BLOCK:(k + 1) * BLOCK]
            img[next_blk * BLOCK:next_blk * BLOCK + len(seg)] = seg
            next_blk += 1
        inodes[i] = inode(1, len(content), blocks)
        root_entries += dirent(name, i)

    inodes[1] = inode(2, len(root_entries), [root_blk])   # 根目录
    img[root_blk * BLOCK:root_blk * BLOCK + len(root_entries)] = root_entries

    # inode 表：1..N 依次摆进 LBA 2..3
    for ino, blob_in in inodes.items():
        off = (INODE_LBA + ino // INODE_PER_BLK) * BLOCK + (ino % INODE_PER_BLK) * 32
        img[off:off + 32] = blob_in

    open(path, "wb").write(img)
    print("fs image: %s" % path)
    print("  total=%d blocks  data=%d  inodes=%d  files=%d  used=%d"
          % (total_blocks, data_blocks, INODE_COUNT, len(files), next_blk - DATA_LBA))


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "/tmp/fs.img")
