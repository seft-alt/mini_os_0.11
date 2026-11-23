#!/usr/bin/env python3
"""tools/datadisk.py —— 在宿主机上造一张 mosfs 数据盘（给 QEMU 挂成 IDE 从盘）

用法：python3 tools/datadisk.py /tmp/fs.img [build/uprog.bin [build/badprog.bin
      [build/uprog.elf [big [build/uprog2.elf [huge [build/uprog3.bin
      [build/uprog4.bin [build/uprog5.bin]]]]]]]] [high=build/uprog6.bin]
      [wx=build/uprog7.elf]
布局严格对齐 include/fs/mosfs.h 里写的偏移；改布局要同时改两处。

位置 1..10 是老约定；第 11 个文件（high）与只读探针（ro，来自 wx=）建议用具名
参数传——一串空位置参数在中间层（SSH 助手、包装脚本）被折叠掉是常事，
一错位它就变成 fd。

只写字节、不碰宿主机的任何分区或设备：产物就是普通文件 /tmp/fs.img。
第二个参数是 build/uprog.bin（boot/uprog.asm 编出来的纯二进制）：
给了就当作 mosfs 里的文件 "uprog"，供 shell 的 exec 读出来在 ring3 跑。
第三个参数是 build/badprog.bin：负向用例（传越界指针、不写指纹），叫 "bad"。
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
    for i, blk in enumerate(blocks[:5]):   # 4 直接 + 1 间接
        b[8 + i * 4:12 + i * 4] = u32(blk)
    return b


def dirent(name, ino):
    b = bytearray(16)
    n = name.encode()[:12]
    b[0:len(n)] = n
    b[12:16] = u32(ino)
    return b


def build(path, total_blocks=2048, uprog_path=None, badprog_path=None,
          elf_path=None, indirect=False, paged_path=None, huge=False,
          slow_path=None, argv_path=None, fd_path=None, high_path=None,
          wx_path=None, part_lba=None, tree=False):
    # part_lba：给了就把整卷布局挪到该 LBA，并在盘首写一张 MBR 指向它。
    # 卷内块号（SB=1、位图=4、数据=5…）一概不变——分区设备的块 0 就是
    # 分区起点，所以内核看到的仍是一张普通 mosfs 盘。
    off = part_lba or 0
    vol = total_blocks - off
    data_blocks = vol - DATA_LBA

    # 三个文件：短文本 / 跨块长文本 / 含不可打印字节的二进制
    hello = b"hello from mosfs v1\nsecond line\nthird line is short\n"
    long_text = ("A" * 78 + "\n").encode()            # 正好 78 列，不触发硬折
    wrapped = ("0123456789" * 9).encode()             # 90 字节单行 -> cat 硬折成两行
    blob = bytes([0x00, 0x01, 0x7F, 0x80, 0xFF]) * 4  # 不可打印，屏上应显示为 '.'
    files = [("hello.txt", hello), ("long.txt", long_text + wrapped),
             ("blob.bin", blob)]
    if indirect:
        # 只给"测间接块"的盘加这两个：3000 字节（6 块 -> 1 个间接块）
        # 和正好 2048 字节（4 个直接块满，没有间接块），
        # 后者留给"写一点 -> 第一次用间接块"这条路径。
        big = "".join("L%03d " % i + "z" * 44 + "\n" for i in range(60)).encode()
        files.append(("big3k", big))
        files.append(("full2k", b"A" * 2048))
    if uprog_path:
        # exec 的示例程序：直接拿构建产物的字节，不在这里手搓机器码
        files.append(("uprog", open(uprog_path, "rb").read()))
    else:
        files.append(("uprog", b"\xF4"))              # 没给就用 hlt 占位，跑不起来的
    if badprog_path:
        # 负向用例：指针越界 + 不写指纹（exec.sh 用它证明门真的在拦）
        files.append(("bad", open(badprog_path, "rb").read()))
    if elf_path:
        # 同一段程序编成 ELF32：exec 应当认头、按 PT_LOAD 搬段、用 e_entry 起跑
        files.append(("uprogel", open(elf_path, "rb").read()))
    if paged_path:
        # 跨两页的 ELF：p_memsz 越过 0x1800，exec 必须给它一段连续两页
        files.append(("uprog2", open(paged_path, "rb").read()))
    if huge:
        # 20KB 的文件：超过 exec 的 16KB 上限，用来测 -2 那条失败码
        files.append(("huge", b"H" * 20000))
    if slow_path:
        # ring3 忙等探针：userpg.sh 趁它在 ring3 的时候采样 TSS.esp0
        files.append(("uprog3", open(slow_path, "rb").read()))
    if argv_path:
        # 参数探针：exec-argv.sh 用它证明 argc/argv 递到了 ring3
        files.append(("argvp", open(argv_path, "rb").read()))
    if fd_path:
        # fd 探针：fd.sh 用它证明 ring3 能 open/read/write/close 盘上的真文件
        files.append(("fdprobe", open(fd_path, "rb").read()))
    if high_path:
        # 高地址探针：pghigh.sh 用它证明"用户态碰 16MB 以上一律当越权"——
        # 这一趟不会把内核拖进 page_fault_map，也不会把系统停住
        files.append(("high", open(high_path, "rb").read()))
    if wx_path:
        # 只读代码段的探针（tools/uprog7.ld 的 FLAGS(5)）：wx.sh 用它证明
        # "ring3 写自己的代码页会被写保护拦成越权"
        files.append(("ro", open(wx_path, "rb").read()))

    img = bytearray(total_blocks * BLOCK)

    sb = bytearray(BLOCK)
    sb[0:4] = MAGIC
    sb[4:8] = u32(BLOCK)
    sb[8:12] = u32(vol)
    sb[12:16] = u32(INODE_COUNT)
    sb[16:20] = u32(1)               # root ino
    sb[20:24] = u32(INODE_LBA)
    sb[24:28] = u32(INODE_PER_BLK)
    sb[28:32] = u32(BITMAP_LBA)
    sb[32:36] = u32(DATA_LBA)
    sb[36:40] = u32(data_blocks)
    img[(off + SB_LBA) * BLOCK:(off + SB_LBA + 1) * BLOCK] = sb

    # 数据块分配：0 号给根目录，之后每个文件按需占用（最多 4 块 = 2KB）
    next_blk = DATA_LBA
    root_blk = next_blk
    next_blk += 1

    root_entries = bytearray()
    inodes = {}
    for i, (name, content) in enumerate(files, start=2):
        nblk = (len(content) + BLOCK - 1) // BLOCK
        assert nblk <= 132, "mosfs v1：4 直接块 + 1 间接块（128）= 132 块"
        blocks = []
        for k in range(nblk):
            blocks.append(next_blk)
            seg = content[k * BLOCK:(k + 1) * BLOCK]
            img[(off + next_blk) * BLOCK:(off + next_blk) * BLOCK + len(seg)] = seg
            next_blk += 1
        if nblk > 4:
            # 超 4 块的部分：拿一块当间接块，里面是接着的块号
            ind = next_blk
            next_blk += 1
            arr = b"".join(u32(b) for b in blocks[4:])
            img[(off + ind) * BLOCK:(off + ind) * BLOCK + len(arr)] = arr
            blocks = blocks[:4] + [ind]
        inodes[i] = inode(1, len(content), blocks)
        root_entries += dirent(name, i)

    if tree:
        # 多级目录：root/d1/{hi.txt, d2/{deep.txt}}。
        # 目录 inode 的 size 就是"有几条 16 字节的目录项"，blocks[0] 是目录块。
        n_ino = len(files) + 2
        d1_blk = next_blk; next_blk += 1
        d2_blk = next_blk; next_blk += 1
        hi_blk = next_blk; next_blk += 1
        deep_blk = next_blk; next_blk += 1

        hi_ino, deep_ino = n_ino, n_ino + 1
        d2_ino, d1_ino = n_ino + 2, n_ino + 3

        hi_txt = b"hi from d1\n"
        deep_txt = b"deep from d2\n"
        img[(off + hi_blk) * BLOCK:(off + hi_blk) * BLOCK + len(hi_txt)] = hi_txt
        img[(off + deep_blk) * BLOCK:(off + deep_blk) * BLOCK
            + len(deep_txt)] = deep_txt

        d2_entries = dirent("deep.txt", deep_ino)
        img[(off + d2_blk) * BLOCK:(off + d2_blk) * BLOCK
            + len(d2_entries)] = d2_entries
        d1_entries = dirent("hi.txt", hi_ino) + dirent("d2", d2_ino)
        img[(off + d1_blk) * BLOCK:(off + d1_blk) * BLOCK
            + len(d1_entries)] = d1_entries

        inodes[hi_ino] = inode(1, len(hi_txt), [hi_blk])
        inodes[deep_ino] = inode(1, len(deep_txt), [deep_blk])
        inodes[d2_ino] = inode(2, len(d2_entries), [d2_blk])
        inodes[d1_ino] = inode(2, len(d1_entries), [d1_blk])
        root_entries += dirent("d1", d1_ino)

    inodes[1] = inode(2, len(root_entries), [root_blk])   # 根目录
    img[(off + root_blk) * BLOCK:(off + root_blk) * BLOCK
        + len(root_entries)] = root_entries

    for ino, blob_in in inodes.items():
        idx = (off + INODE_LBA + ino // INODE_PER_BLK) * BLOCK \
            + (ino % INODE_PER_BLK) * 32
        img[idx:idx + 32] = blob_in

    # 位图：bit i = 块号 i 被占用。造盘时把上面真正用掉的块都记上，
    # 否则内核的写路径会以为根目录块还是空的，第一次 append 就把它覆盖掉。
    bm = bytearray(BLOCK)
    for b in range(DATA_LBA, next_blk):
        bm[b // 8] |= 1 << (b % 8)
    img[(off + BITMAP_LBA) * BLOCK:(off + BITMAP_LBA + 1) * BLOCK] = bm
    free_blk = data_blocks - (next_blk - DATA_LBA)

    if off:
        # MBR：分区表项从 446 起，第 1 条指向本卷；末尾 0x55 0xAA
        ent = bytearray(16)
        ent[0] = 0x80                    # 可引导标志（不要求内核认，只是像真盘）
        ent[4] = 0x83                    # Linux 类型
        ent[8:12] = u32(off)             # 起始 LBA
        ent[12:16] = u32(vol)            # 扇区数
        img[446:462] = ent
        img[510] = 0x55
        img[511] = 0xAA

    open(path, "wb").write(img)
    print("fs image: %s" % path)
    if off:
        print("  mbr: part1 start=%d count=%d（整盘共 %d 块）"
              % (off, vol, total_blocks))
    print("  total=%d blocks  data=%d  inodes=%d  files=%d  used=%d  free=%d"
          % (vol, data_blocks, INODE_COUNT, len(files),
             next_blk - DATA_LBA, free_blk))


if __name__ == "__main__":
    # 位置参数 + `名字=值` 形式的具名参数混用：
    #   位置 1..10 是老约定（img uprog bad elf indirect paged huge slow argv fd），
    #   具名参数给"前面一堆空位"的调用方用——空串在中间层（ssh 助手、shell
    #   包装脚本）被折叠掉是常事，那时第 11 个位置就会整体错位（high 被当成 fd）。
    named = {}
    pos = []
    for a in sys.argv[1:]:
        if "=" in a:
            k, v = a.split("=", 1)
            named[k] = v
        else:
            pos.append(a)
    img = pos[0] if len(pos) > 0 else "/tmp/fs.img"
    uprog = pos[1] if len(pos) > 1 else None
    bad = pos[2] if len(pos) > 2 else None
    elfp = pos[3] if len(pos) > 3 else None
    indirect = len(pos) > 4 and pos[4] == "big"
    paged = pos[5] if len(pos) > 5 else None
    huge = len(pos) > 6 and pos[6] == "huge"
    slow = pos[7] if len(pos) > 7 else None
    argvp = pos[8] if len(pos) > 8 else None
    fdp = pos[9] if len(pos) > 9 else None
    highp = named.get("high")
    if highp is None and len(pos) > 10:
        highp = pos[10]
    wxp = named.get("wx")
    partp = named.get("part")            # part=64 → 卷从 LBA 64 起，盘首写 MBR
    treep = named.get("tree") == "1"     # tree=1 → 多造一棵 /d1/{hi.txt,d2/deep.txt}
    build(img, uprog_path=uprog, badprog_path=bad, elf_path=elfp,
          indirect=indirect, paged_path=paged, huge=huge, slow_path=slow,
          argv_path=argvp, fd_path=fdp, high_path=highp, wx_path=wxp,
          part_lba=int(partp) if partp else None, tree=treep)
