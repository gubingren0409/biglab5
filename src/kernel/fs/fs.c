#include "mod.h"
super_block_t sb;
/* 文件系统初始化
    1. 初始化底层设备和缓存
    2. 运行测试用例
*/
void fs_init()
{
    printf("fs_init ... \n");

    virtio_disk_init();
    buffer_init();
	// --- 新增：读取超级块 (Block 0) ---
    buffer_t *b = buffer_get(FS_SB_BLOCK);
    // 将磁盘数据拷贝到内存中的 sb 结构体
    memcpy(&sb, b->data, sizeof(super_block_t));
    buffer_put(b);

    // 简单检查一下魔数，确保读到了正确的文件系统
    if (sb.magic_num != FS_MAGIC) {
        panic("fs_init: invalid magic number");
    }
    printf("fs_init: magic check pass!\n");
    // -------------------------------
    inode_init(); // 初始化 inode 子系统

    // 以下是 README 提供的标准测试逻辑
    // 你可以根据需要注释掉部分测试，逐个通过

    printf("============= test begin =============\n\n");

    /* --- 测试1: inode的访问 + 创建 + 删除 --- */
    printf("--- Test 1 ---\n");
    inode_t *rooti, *ip_1, *ip_2;
    
    rooti = inode_get(ROOT_INODE);
    inode_lock(rooti);
    inode_print(rooti, "root");
    inode_unlock(rooti);

    /* 第一次查看bitmap */
    bitmap_print(false);

    ip_1 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    ip_2 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    inode_lock(ip_1);
    inode_lock(ip_2);
    inode_dup(ip_2); // 增加引用计数

    inode_print(ip_1, "dir");
    inode_print(ip_2, "data");
    
    /* 第二次查看bitmap */
    bitmap_print(false);

    // 模拟删除
    ip_1->disk_info.nlink = 0;
    ip_2->disk_info.nlink = 0;
    inode_unlock(ip_1);
    inode_unlock(ip_2);
    inode_put(ip_1);
    inode_put(ip_2); // ref 2->1

    /* 第三次查看bitmap */
    bitmap_print(false);

    inode_put(ip_2); // ref 1->0, 触发真实删除
    
    /* 第四次查看bitmap */
    bitmap_print(false);


    /* --- 测试2: 写入和读取inode管理的数据 --- */
    printf("\n--- Test 2 ---\n");
    uint32 len, cut_len;

    /* 小批量读写测试 */
    int small_src[10], small_dst[10];
    for (int i = 0; i < 10; i++)
        small_src[i] = i;
    
    ip_1 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    inode_lock(ip_1);
    inode_print(ip_1, "small_data");

    printf("writing data...\n");
    cut_len = 10 * sizeof(int);
    // 写入 400 次
    for (uint32 offset = 0; offset < 400 * cut_len; offset += cut_len) {
        len = inode_write_data(ip_1, offset, cut_len, small_src, false);
        if(len != cut_len) panic("write fail 1!");
    }
    inode_print(ip_1, "small_data");

    len = inode_read_data(ip_1, 120 * cut_len + 4, cut_len, small_dst, false);
    if(len != cut_len) panic("read fail 1!");
    printf("read data:");
    for (int i = 0; i < 10; i++)
        printf(" %d", small_dst[i]);
    printf("\n");

    ip_1->disk_info.nlink = 0;
    inode_unlock(ip_1);
    inode_put(ip_1);

    /* 大批量读写测试 (大文件) */
    static char big_src_buf[5 * 4096]; // 定义一个足够大的静态数组 (20KB)
    char *big_src = big_src_buf;
    
    char big_dst[9];
    big_dst[8] = 0;

    for (uint32 i = 0; i < 5 * (PGSIZE / 8); i++)
        for (uint32 j = 0; j < 8; j++)
            big_src[i * 8 + j] = 'A' + j;

    ip_2 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    inode_lock(ip_2);
    inode_print(ip_2, "big_data");

    printf("writing big data... (wait)\n");
    cut_len = PGSIZE * 4 + 1110;
    for (uint32 offset = 0; offset < cut_len * 10000; offset += cut_len)
    {
        len = inode_write_data(ip_2, offset, cut_len, big_src, false);
        if(len != cut_len) panic("write fail 2!");
    }
    inode_print(ip_2, "big_data");

    len = inode_read_data(ip_1, cut_len * 10000 - 8, 8, big_dst, false); // 注意这里原代码是ip_1? 应该是 ip_2
    // 助教代码可能有笔误，原README写的是 ip_1，但根据上下文应该是读刚才写的 ip_2
    // 如果原README是 ip_1，那是测试把 ip_1 删除后还能不能读？
    // 不，ip_1 已经被删除了。这里应该是 ip_2。我修正为 ip_2。
    len = inode_read_data(ip_2, cut_len * 10000 - 8, 8, big_dst, false);

    if(len != 8) panic("read fail 2");
    printf("read data: %s\n", big_dst);

    ip_2->disk_info.nlink = 0;
    inode_unlock(ip_2);
    inode_put(ip_2);
    


    /* --- 测试3: 目录项的增加、删除、查找操作 --- */
    printf("\n--- Test 3 ---\n");
    inode_t *ip_3;
    uint32 inode_num_1, inode_num_2, inode_num_3;
    uint32 offset;
    char tmp[10];

    tmp[9] = 0;
    cut_len = 9;
    rooti = inode_get(ROOT_INODE);

    /* 搜索预置的dentry */
    inode_lock(rooti);
    inode_num_1 = dentry_search(rooti, "ABCD.txt");
    inode_num_2 = dentry_search(rooti, "abcd.txt");
    inode_num_3 = dentry_search(rooti, ".");
    
    if (inode_num_1 == INVALID_INODE_NUM || 
        inode_num_2 == INVALID_INODE_NUM || 
        inode_num_3 == INVALID_INODE_NUM) {
        panic("invalid inode num!");
    }
    dentry_print(rooti);
    inode_unlock(rooti);

    ip_1 = inode_get(inode_num_1);
    inode_lock(ip_1);
    ip_2 = inode_get(inode_num_2);
    inode_lock(ip_2);
    ip_3 = inode_get(inode_num_3);
    inode_lock(ip_3);

    inode_print(ip_1, "ABCD.txt");
    inode_print(ip_2, "abcd.txt");
    inode_print(ip_3, "root");

    len = inode_read_data(ip_1, 0, cut_len, tmp, false);
    if(len != cut_len) panic("read fail 1!");
    printf("\nread data: %s\n", tmp);

    len = inode_read_data(ip_2, 0, cut_len, tmp, false);
    if(len != cut_len) panic("read fail 2!");
    printf("read data: %s\n\n", tmp);

    inode_unlock(ip_1);
    inode_unlock(ip_2);
    inode_unlock(ip_3);
    inode_put(ip_1);
    inode_put(ip_2);
    inode_put(ip_3);

    /* 创建和删除dentry */
    inode_lock(rooti);

    ip_1 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);    
    offset = dentry_create(rooti, ip_1->inode_num, "new_dir");
    inode_num_1 = dentry_search(rooti, "new_dir");
    printf("new dentry offset = %d\n", offset);
    printf("new dentry inode_num = %d\n\n", inode_num_1);
    
    dentry_print(rooti);

    inode_num_2 = dentry_delete(rooti, "new_dir");
    if(inode_num_1 != inode_num_2) panic("inode num is not equal!");

    dentry_print(rooti);

    inode_unlock(rooti);
    inode_put(rooti);


    /* --- 测试4: 文件路径的解析 --- */
    printf("\n--- Test 4 ---\n");
    inode_t *ip_4, *ip_5;
    
    rooti = inode_get(ROOT_INODE);
    ip_1 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    ip_2 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    ip_3 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    
    inode_lock(rooti);
    inode_lock(ip_1);
    inode_lock(ip_2);
    inode_lock(ip_3);

    if (dentry_create(rooti, ip_1->inode_num, "AABBC") == -1)
        panic("dentry_create fail 1!");
    if (dentry_create(ip_1, ip_2->inode_num, "aaabb") == -1)
        panic("dentry_create fail 2!");
    if (dentry_create(ip_2, ip_3->inode_num, "file.txt") == -1)
        panic("dentry_create fail 3!");

    char tmp1[] = "This is file context!";
    char tmp2[32];
    inode_write_data(ip_3, 0, sizeof(tmp1), tmp1, false);

    inode_rw(rooti, true);
    inode_rw(ip_1, true);
    inode_rw(ip_2, true);
	inode_rw(ip_3, true);

    inode_unlock(rooti);
    inode_unlock(ip_1);
    inode_unlock(ip_2);
    inode_unlock(ip_3);
    inode_put(rooti);
    inode_put(ip_1);
    inode_put(ip_2);
    inode_put(ip_3);

    char *path = "///AABBC///aaabb/file.txt";
    char name[MAXLEN_FILENAME];

    ip_4 = path_to_inode(path);
    if (ip_4 == NULL)
        panic("invalid ip_4");

    ip_5 = path_to_parent_inode(path, name);
    if (ip_5 == NULL)
        panic("invalid ip_5");
    
    printf("get a name = %s\n\n", name);

    inode_lock(ip_4);
    inode_lock(ip_5);

    inode_print(ip_4, "file.txt");
    inode_print(ip_5, "aaabb");

    inode_read_data(ip_4, 0, 32, tmp2, false);
    printf("read data: %s\n\n", tmp2);

    inode_unlock(ip_4);
    inode_unlock(ip_5);
    inode_put(ip_4);
    inode_put(ip_5);

    printf("============= test end =============\n");

    while(1);
}