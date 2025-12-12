LAB-8 实验报告：文件系统实现

1. 实验概述

本实验旨在操作系统内核中构建一个完整的文件系统。在 Lab-7 实现的块设备驱动（Block Driver）和缓冲区高速缓存（Buffer Cache）的基础上，本次实验向上抽象出了 inode（索引节点） 和 dentry（目录项） 两个核心概念，实现了从底层无结构的磁盘块到上层层次化文件树的跨越。

核心工作包括：



磁盘布局设计：通过 mkfs 工具初始化磁盘映像。

数据组织：实现 inode 的多级索引机制（直接/一级间接/二级间接），支持大文件存储。

生命周期管理：实现内存 inode 缓存池（Inode Cache），处理引用计数与并发锁。

目录管理：实现目录项（dentry）的增删查，构建文件名到 inode 号的映射。

路径解析：实现绝对路径解析逻辑，支持多级目录跳转。

2. 核心模块实现详解

2.1 磁盘映像初始化 (mkfs)

为了让内核启动时有一个可用的文件系统，我们编写了 mkfs 工具在用户态生成初始的磁盘映像 disk.img。



磁盘布局：[ Super Block | Inode Bitmap | Inode Blocks | Data Bitmap | Data Blocks ]

根目录构建：初始化了根目录（inode 0），并预置了 . 和 .. 目录项。

测试文件：预置了 ABCD.txt 和 abcd.txt 用于后续的读写测试。

2.2 索引节点 (Inode) 与数据组织

Inode 是文件系统的核心元数据结构。我们在 src/kernel/fs/inode.c 中实现了其管理逻辑。



2.2.1 多级索引机制

为了兼顾小文件的访问效率和大文件的存储能力，我们实现了混合索引机制：



直接映射 (index[0-9])：直接指向数据块，支持 40KB 以下的小文件快速访问。

一级间接映射 (index[10-11])：指向索引块，每个索引块存储 1024 个指针，支持 8MB 级文件。

二级间接映射 (index[12])：指向“索引的索引”，支持 4GB 级的大文件。

实现函数：locate_or_add_block。该函数负责将逻辑块号（Logical Block Number）翻译为物理块号，并在写入时自动分配缺失的磁盘块（包括数据块和中间索引块）。



2.2.2 内存 Inode 缓存 (Inode Cache)

为了减少磁盘 I/O，我们在内存中维护了一个 inode_cache 池。



结构设计：区分 inode_disk_t（磁盘持久化数据）和 inode_t（内存运行时数据）。

同步机制：

自旋锁 (lk_inode_cache)：保护缓存池的分配和引用计数 (ref)。

睡眠锁 (slk)：保护 inode 的具体内容（如 size, index）读写，允许进程在等待磁盘 I/O 时睡眠。

生命周期：

inode_get：查找缓存或分配空闲槽位，增加引用计数。

inode_put：减少引用计数。当 ref=0 且 nlink=0（硬链接数为0）时，触发 inode_delete 彻底回收磁盘资源。

2.3 目录项 (Dentry)

Dentry 负责实现“文件名字符串”到“inode 编号”的映射，从而构建出人类可读的目录树。我们在 src/kernel/fs/dentry.c 中实现了相关逻辑。



存储格式：目录被视为一种特殊的文件，其数据块中存储的是 dentry_t 结构体数组。

核心操作：

dentry_search：线性扫描目录的数据块，查找匹配文件名的 inode 号。

dentry_create：在目录中寻找空闲槽位（或追加新块）写入新的目录项。

dentry_delete：将指定文件名的目录项清零，标记为未使用。

2.4 路径解析 (Path Resolution)

基于 Dentry，我们实现了从绝对路径（如 /home/user/file.txt）找到目标 Inode 的逻辑。



解析流程：从根目录 (ROOT_INODE) 开始，利用 get_element 提取各级文件名，循环调用 dentry_search 逐级向下查找。

父目录解析：实现了 path_to_parent_inode，用于在创建新文件时获取目标路径父目录的 inode。

2.4.1 关键 Bug 修复：路径解析死锁

在开发 path_to_parent_inode 时，我们遇到了严重的死锁问题（Test 4 卡死）。



问题现象：在解析如 .../dir/file 时，函数错误地将 file 当作目录继续进入，导致返回了 file 的 inode 而非 dir 的 inode。后续代码试图对同一个 inode 加锁两次（先锁子节点，后锁父节点，但二者变成了同一个），触发死锁。

解决方案：修正了循环终止条件。通过检查 get_element 的返回值（剩余路径指针）是否为 NULL 来准确判断路径是否结束，确保正确返回父目录的 inode。

3. 测试验证

我们通过内核启动时的自检程序 (fs_init) 完成了四个阶段的测试，所有测试均通过。



Test 1: Inode 基础操作

验证了 inode_create、inode_dup 和 inode_put。

确认了引用计数归零后，位图（Bitmap）能正确回收 Inode 资源。

Test 2: 数据读写

小文件：验证了直接索引的读写正确性。

大文件：写入了跨越直接映射和间接映射的大型数据（约 170MB 虚拟大小），验证了 locate_or_add_block 中多级索引分配逻辑的正确性。

Test 3: 目录操作

验证了 dentry_search 能正确找到预置文件。

验证了 dentry_create 和 dentry_delete 能正确管理目录槽位。

Test 4: 路径解析

验证了 path_to_inode 和 path_to_parent_inode 能正确解析多级路径 ///AABBC///aaabb/file.txt。

成功读取了通过路径写入的文件内容，证明文件系统层次结构功能完备。

4. 总结

Lab 8 成功实现了一个类 Unix 的简单文件系统。它不仅支持基础的数据持久化，还通过完善的 inode 缓存和锁机制保证了内核态的并发安全，通过多级索引支持了较大规模的文件存储，为后续 Lab 9（文件描述符与设备文件）和进程 exec 的实现奠定了坚实基础。