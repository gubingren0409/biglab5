#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include "mkfs.h"

static int disk_fd;                 // 磁盘映像的文件描述符
static super_block_t sb;            // 超级块
static int global_inode_num = 0;    // 全局的inode_num
static int global_block_num = 0;    // 全局的block_num

static char bitmap_buf[BLOCK_SIZE]; // bitmap区域的读写缓冲
static char inode_buf[BLOCK_SIZE];  // inode区域的读写缓冲
static char data_buf[BLOCK_SIZE];   // data区的读写缓冲

/*-------------------------关于小端序-------------------------*/

unsigned short xshort(unsigned short x) {
    unsigned short y;
    unsigned char* a = (unsigned char*)&y;
    a[0] = x; a[1] = x >> 8;
    return y;
}

unsigned int xint(unsigned int x) {
    unsigned int y;
    unsigned char* a = (unsigned char*)&y;
    a[0] = x; a[1] = x >> 8; a[2] = x >> 16; a[3] = x >> 24;
    return y;
}

/*-------------------------磁盘区域读写能力-------------------------*/

void block_rw(unsigned int block_num, void *buf, bool write_it) {
    if (lseek(disk_fd, BLOCK_SIZE * block_num, 0) != BLOCK_SIZE * block_num) {
        perror("lseek"); exit(1);
    }
    if (write_it) {
        if (write(disk_fd, buf, BLOCK_SIZE) != BLOCK_SIZE) { perror("write"); exit(1); }
    } else {
        if (read(disk_fd, buf, BLOCK_SIZE) != BLOCK_SIZE) { perror("read"); exit(1); }
    }
}

void inode_rw(unsigned int inode_num, inode_disk_t *ip, bool write_it) {
    unsigned int block_num = sb.inode_firstblock + inode_num / INODE_PER_BLOCK;
    unsigned int byte_offset = (inode_num % INODE_PER_BLOCK) * sizeof(inode_disk_t);
    if (write_it) {
        block_rw(block_num, inode_buf, false);
        memcpy(inode_buf + byte_offset, ip, sizeof(inode_disk_t));
        block_rw(block_num, inode_buf, true);
    } else {
        block_rw(block_num, inode_buf, false);
        memcpy(ip, inode_buf + byte_offset, sizeof(inode_disk_t));
    }
}

unsigned int block_alloc() {
    unsigned int block_num, byte_offset, bit_offset;
    block_num = sb.data_bitmap_firstblock + global_block_num / BIT_PER_BLOCK;
    bit_offset = global_block_num % BIT_PER_BLOCK;
    byte_offset = bit_offset / BIT_PER_BYTE;
    bit_offset = bit_offset % BIT_PER_BYTE;
    block_rw(block_num, bitmap_buf, false);
    bitmap_buf[byte_offset] |= (1 << bit_offset);
    block_rw(block_num, bitmap_buf, true);
    return sb.data_firstblock + global_block_num++;
}

unsigned int inode_alloc() {
    unsigned int block_num, byte_offset, bit_offset;
    block_num = sb.inode_bitmap_firstblock + global_inode_num / BIT_PER_BLOCK;
    bit_offset = global_inode_num % BIT_PER_BLOCK;
    byte_offset = bit_offset / BIT_PER_BYTE;
    bit_offset = bit_offset % BIT_PER_BYTE;
    block_rw(block_num, bitmap_buf, false);
    bitmap_buf[byte_offset] |= (1 << bit_offset);
    block_rw(block_num, bitmap_buf, true);
    return global_inode_num++;
}

/*-------------------------inode精细化管理-------------------------*/

void inode_init(inode_disk_t *ip, short type, short major, short minor) {
    ip->type = type;
    ip->major = major;
    ip->minor = minor;
    ip->nlink = 1;
    ip->size = 0;
    for (int i = 0; i < INODE_INDEX_3; i++) ip->index[i] = 0;
}

void inode_append(inode_disk_t *ip, void *data, unsigned int len) {
    unsigned int old_blocks = COUNT_BLOCKS(ip->size, BLOCK_SIZE);
    unsigned int new_blocks = COUNT_BLOCKS(ip->size + len, BLOCK_SIZE);
    unsigned int tmp = ip->size / BLOCK_SIZE;
    unsigned int tar_len = len;
    char *data_new = (char*)data; 

    if (new_blocks > old_blocks) {
        if (new_blocks > INODE_BLOCK_INDEX_1) {
            printf("inode_append: data len out of space!\n"); return;
        }
        for(int i = old_blocks; i < new_blocks; i++) ip->index[i] = block_alloc();
    }

    while (len > 0) {
        unsigned int cut_len;
        if (tmp == ip->size / BLOCK_SIZE) { 
            unsigned int offset = ip->size % BLOCK_SIZE;
            cut_len = MIN(BLOCK_SIZE - offset, len);
            block_rw(ip->index[tmp], data_buf, false);
            memcpy(data_buf + offset, data_new, cut_len);
        } else {
            cut_len = MIN(BLOCK_SIZE, len);
            memcpy(data_buf, data_new, cut_len);
        }
        block_rw(ip->index[tmp], data_buf, true);
        len -= cut_len;
        data_new += cut_len;
        tmp++;
    }
    ip->size += tar_len;
}

/* * 构造的文件系统结构:
 * / (root)
 * ├── dev
 * │   ├── stdin
 * │   ├── stdout
 * │   └── stderr
 * ├── ABCD.txt
 * └── abcd.txt
 */
int main(int argc, char* argv[])
{
    assert(BLOCK_SIZE % sizeof(inode_disk_t) == 0);

    inode_disk_t inode[7];
    unsigned int inode_num[7];
    dentry_t de;

	/* step-1: 填充 superblock */
	sb.magic_num = FS_MAGIC;
	sb.block_size = BLOCK_SIZE;
	sb.inode_bitmap_firstblock = 1;
	sb.inode_bitmap_blocks = COUNT_BLOCKS(N_INODE, BIT_PER_BLOCK);
	sb.inode_firstblock = sb.inode_bitmap_firstblock + sb.inode_bitmap_blocks;
	sb.inode_blocks = COUNT_BLOCKS(N_INODE, INODE_PER_BLOCK);
	sb.data_bitmap_firstblock = sb.inode_firstblock + sb.inode_blocks;
	sb.data_bitmap_blocks = COUNT_BLOCKS(N_DATA_BLOCK, BIT_PER_BLOCK);
	sb.data_firstblock = sb.data_bitmap_firstblock + sb.data_bitmap_blocks;
	sb.data_blocks = N_DATA_BLOCK;
    sb.total_inodes = N_INODE;
	sb.total_blocks = 1 + sb.inode_bitmap_blocks + sb.inode_blocks
			+ sb.data_bitmap_blocks + sb.data_blocks;

    /* step-2: 创建磁盘文件 */
    disk_fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
    if(disk_fd < 0) { perror(argv[1]); exit(1); }

    /* step-3: 清零磁盘映像 */
    memset(data_buf, 0, BLOCK_SIZE);
    printf("\nPreparing disk.img...\n\n");
    // [修复] 恢复完整初始化，确保所有块（特别是元数据区域）都被写入
    // 只要 mkfs.h 中 N_DATA_BLOCK 设置合理（如 128MB），这个循环很快就能完成
    for(int i = 0; i < sb.total_blocks; i++) 
        block_rw(i, data_buf, true);

    /* step-4: 制作根目录 (Inode 0) */
    inode_num[0] = inode_alloc(); // 0
    inode_init(&inode[0], INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    
    // 写入 . 和 ..
    de.inode_num = xint(inode_num[0]); strcpy(de.name, ".");
    inode_append(&inode[0], &de, sizeof(dentry_t));
    de.inode_num = xint(inode_num[0]); strcpy(de.name, "..");
    inode_append(&inode[0], &de, sizeof(dentry_t));

    /* step-5: 制作 /dev 目录 (Inode 1) */
    inode_num[1] = inode_alloc(); // 1
    inode_init(&inode[1], INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    
    // 写入 /dev/. 和 /dev/..
    de.inode_num = xint(inode_num[1]); strcpy(de.name, ".");
    inode_append(&inode[1], &de, sizeof(dentry_t));
    de.inode_num = xint(inode_num[0]); strcpy(de.name, "..");
    inode_append(&inode[1], &de, sizeof(dentry_t));

    // 将 dev 挂载到根目录
    de.inode_num = xint(inode_num[1]); strcpy(de.name, "dev");
    inode_append(&inode[0], &de, sizeof(dentry_t));

    /* step-6: 制作设备文件 (Inode 2, 3, 4) */
    // stdin
    inode_num[2] = inode_alloc(); // 2
    inode_init(&inode[2], INODE_TYPE_DEVICE, INODE_MAJOR_STDIN, 0);
    de.inode_num = xint(inode_num[2]); strcpy(de.name, "stdin");
    inode_append(&inode[1], &de, sizeof(dentry_t));

    // stdout
    inode_num[3] = inode_alloc(); // 3
    inode_init(&inode[3], INODE_TYPE_DEVICE, INODE_MAJOR_STDOUT, 0);
    de.inode_num = xint(inode_num[3]); strcpy(de.name, "stdout");
    inode_append(&inode[1], &de, sizeof(dentry_t));

    // stderr
    inode_num[4] = inode_alloc(); // 4
    inode_init(&inode[4], INODE_TYPE_DEVICE, INODE_MAJOR_STDERR, 0);
    de.inode_num = xint(inode_num[4]); strcpy(de.name, "stderr");
    inode_append(&inode[1], &de, sizeof(dentry_t));

    /* step-7: 制作测试文本文件 (Inode 5, 6) */
    inode_num[5] = inode_alloc();
    inode_init(&inode[5], INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    de.inode_num = xint(inode_num[5]); strcpy(de.name, "ABCD.txt");
    inode_append(&inode[0], &de, sizeof(dentry_t));

    inode_num[6] = inode_alloc();
    inode_init(&inode[6], INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    de.inode_num = xint(inode_num[6]); strcpy(de.name, "abcd.txt");
    inode_append(&inode[0], &de, sizeof(dentry_t));

    // 填充文件内容
    char tmp[26];
    for (int i = 0; i < 26; i++) tmp[i] = 'A' + i;
    for (int i = 0; i < 200; i++) inode_append(&inode[5], tmp, sizeof(tmp));
    for (int i = 0; i < 26; i++) tmp[i] = 'a' + i;
    for (int i = 0; i < 500; i++) inode_append(&inode[6], tmp, sizeof(tmp));

    /* step-8: 写回 super block 和所有 inode */
    sb.magic_num = xint(sb.magic_num);
    sb.block_size = xint(sb.block_size);
    sb.inode_bitmap_blocks = xint(sb.inode_bitmap_blocks);
    sb.inode_bitmap_firstblock = xint(sb.inode_bitmap_firstblock);
    sb.inode_blocks = xint(sb.inode_blocks);
    sb.inode_firstblock = xint(sb.inode_firstblock);
    sb.data_bitmap_blocks = xint(sb.data_bitmap_blocks);
    sb.data_bitmap_firstblock = xint(sb.data_bitmap_firstblock);
    sb.data_blocks = xint(sb.data_blocks);
    sb.data_firstblock = xint(sb.data_firstblock);
    sb.total_inodes = xint(sb.total_inodes);
    sb.total_blocks = xint(sb.total_blocks);
    memcpy(data_buf, &sb, sizeof(sb));
    block_rw(0, data_buf, true);

    for (int i = 0; i < 7; i++) {
        inode[i].type = xshort(inode[i].type);
        inode[i].major = xshort(inode[i].major);
        inode[i].minor = xshort(inode[i].minor);
        inode[i].nlink = xshort(inode[i].nlink);
        inode[i].size = xint(inode[i].size);
        for(int j = 0; j < INODE_INDEX_3; j++)
            inode[i].index[j] = xint(inode[i].index[j]);
        inode_rw(inode_num[i], &inode[i], true);
    }

    close(disk_fd);
    return 0;
}