#include "mod.h"

#define O_CREATE    FILE_OPEN_CREATE
#define O_RDONLY    FILE_OPEN_READ
#define O_WRONLY    FILE_OPEN_WRITE
#define O_RDWR      (FILE_OPEN_READ | FILE_OPEN_WRITE)
#define O_APPEND    0x08 

// 映射 Inode 类型
#define INODE_FILE      INODE_TYPE_DATA
#define INODE_DIRECTORY INODE_TYPE_DIR
#define INODE_DEVICE    INODE_TYPE_DEVICE

// 全局文件表：存放所有打开的文件对象
file_t file_table[N_FILE];
spinlock_t lk_file_table;

// [修复] 定义超级块变量
super_block_t sb;

// [修复] 实现辅助拷贝函数 (从 device.c 移植或包含)
static int either_copy_to(bool is_user, uint64 dst, void *src, uint32 len) {
    if (is_user) {
        uvm_copyout(myproc()->pgtbl, dst, (uint64)src, len);
    } else {
        memmove((void*)dst, src, len);
    }
    return 0;
}

/**
 * 第1步：初始化文件系统顶层抽象
 */
void file_init() {
    // [修复] 函数名 spinlock_init
    spinlock_init(&lk_file_table, "file_table");
    spinlock_acquire(&lk_file_table);
    for (int i = 0; i < N_FILE; i++) {
        file_table[i].ref = 0;
    }
    spinlock_release(&lk_file_table);
}

/**
 * 初始化整个文件系统模块
 */
void fs_init() {
    printf("fs_init ... \n");

    // 1. 底层驱动与缓存初始化
    virtio_disk_init();
    buffer_init();

    // 2. 加载超级块
    buffer_t *b = buffer_get(FS_SB_BLOCK);
    memcpy(&sb, b->data, sizeof(super_block_t));
    buffer_put(b);

    if (sb.magic_num != FS_MAGIC) {
        panic("fs_init: invalid magic number");
    }

    // 3. 层次结构初始化
    inode_init();   // inode 管理层
    file_init();    // 文件抽象层
    device_init();  // 设备文件初始化 [修复: 已在 method.h 声明]

    printf("fs_init: done. Magic 0x%x\n", sb.magic_num);
}

/**
 * 从全局文件表中分配一个空闲的 file 结构
 */
file_t* file_alloc() {
    spinlock_acquire(&lk_file_table);
    for (int i = 0; i < N_FILE; i++) {
        if (file_table[i].ref == 0) {
            file_table[i].ref = 1;
            file_table[i].offset = 0;
            file_table[i].readable = false;
            file_table[i].writable = false;
            file_table[i].ip = NULL;
            spinlock_release(&lk_file_table);
            return &file_table[i];
        }
    }
    spinlock_release(&lk_file_table);
    return NULL;
}

/**
 * 增加文件的引用计数
 */
file_t* file_dup(file_t* f) {
    spinlock_acquire(&lk_file_table);
    if (f->ref < 1) panic("file_dup");
    f->ref++;
    spinlock_release(&lk_file_table);
    return f;
}

/**
 * 关闭文件
 */
void file_close(file_t *f) {
    spinlock_acquire(&lk_file_table);
    if (f->ref < 1) panic("file_close");
    if (--f->ref > 0) {
        spinlock_release(&lk_file_table);
        return;
    }
    
    inode_t *ip = f->ip;
    f->ref = 0;
    f->ip = NULL;
    spinlock_release(&lk_file_table);

    if (ip) {
        inode_put(ip);
    }
}

/**
 * 打开文件
 */
file_t* file_open(char *path, uint32 open_mode) {
    inode_t *ip;

    if (open_mode & O_CREATE) {
        ip = path_create_inode(path, INODE_FILE, 0, 0);
    } else {
        ip = __path_to_inode(NULL, path, false);
    }

    if (ip == NULL) return NULL;

    // [修复] 函数名 sleeplock_acquire
    sleeplock_acquire(&ip->slk);

    if (ip->disk_info.type == INODE_DIRECTORY && (open_mode != O_RDONLY)) {
        sleeplock_release(&ip->slk);
        inode_put(ip);
        return NULL;
    }

    if (ip->disk_info.type == INODE_DEVICE) {
        // [修复] device_open_check 已在 method.h 声明
        if (!device_open_check(ip->disk_info.major, open_mode)) {
            sleeplock_release(&ip->slk);
            inode_put(ip);
            return NULL;
        }
    }

    file_t *f = file_alloc();
    if (f == NULL) {
        sleeplock_release(&ip->slk);
        inode_put(ip);
        return NULL;
    }

    f->ip = ip;
    f->readable = (open_mode & O_WRONLY) ? false : true;
    f->writable = (open_mode & O_WRONLY) || (open_mode & O_RDWR);
    
    if ((open_mode & O_APPEND) && ip->disk_info.type == INODE_FILE) {
        f->offset = ip->disk_info.size;
    } else {
        f->offset = 0;
    }

    sleeplock_release(&ip->slk);
    return f;
}

/**
 * 读取文件
 */
uint32 file_read(file_t* f, uint32 len, uint64 dst, bool is_user_dst) {
    if (!f->readable) return -1;

    uint32 bytes = 0;
    if (f->ip->disk_info.type == INODE_DEVICE) {
        // [修复] device_read_data 已声明
        bytes = device_read_data(f->ip->disk_info.major, len, dst, is_user_dst);
    } else {
        sleeplock_acquire(&f->ip->slk);
        // [修复] 使用 inode_read_data，并调整参数顺序 (offset, len, dst)
        bytes = inode_read_data(f->ip, f->offset, len, (void*)dst, is_user_dst);
        if (bytes > 0) {
            f->offset += bytes;
        }
        sleeplock_release(&f->ip->slk);
    }
    return bytes;
}

/**
 * 写入文件
 */
uint32 file_write(file_t* f, uint32 len, uint64 src, bool is_user_src) {
    if (!f->writable) return -1;

    uint32 bytes = 0;
    if (f->ip->disk_info.type == INODE_DEVICE) {
        bytes = device_write_data(f->ip->disk_info.major, len, src, is_user_src);
    } else {
        sleeplock_acquire(&f->ip->slk);
        // [修复] 使用 inode_write_data，并调整参数顺序 (offset, len, src)
        bytes = inode_write_data(f->ip, f->offset, len, (void*)src, is_user_src);
        if (bytes > 0) {
            f->offset += bytes;
        }
        sleeplock_release(&f->ip->slk);
    }
    return bytes;
}

/**
 * 移动读写指针
 */
uint32 file_lseek(file_t *f, uint32 lseek_offset, uint32 lseek_flag) {
    sleeplock_acquire(&f->ip->slk);
    uint32 new_off = f->offset;

    // [修复] LSEEK_SET 等宏现在已在 type.h 定义
    switch (lseek_flag) {
        case LSEEK_SET:
            new_off = lseek_offset;
            break;
        case LSEEK_CUR:
            new_off += lseek_offset;
            break;
        case LSEEK_END:
            new_off = f->ip->disk_info.size + lseek_offset;
            break;
        default:
            sleeplock_release(&f->ip->slk);
            return -1;
    }

    f->offset = new_off;
    sleeplock_release(&f->ip->slk);
    return new_off;
}

/**
 * 获取文件状态信息 (stat)
 */
uint32 file_get_stat(file_t* f, uint64 user_dst) {
    // [修复] 类型改为 file_stat_t (在 type.h 中定义)
    file_stat_t st;
    sleeplock_acquire(&f->ip->slk);
    
    // [修复] 成员名 inum -> inode_num
    st.inode_num = f->ip->inode_num;
    st.type = f->ip->disk_info.type;
    // [修复] nlink 位于 disk_info 中
    st.nlink = f->ip->disk_info.nlink; 
    st.size = f->ip->disk_info.size;
    st.offset = f->offset;
    
    sleeplock_release(&f->ip->slk);
    
    // [修复] either_copy_to 现已定义
    if (either_copy_to(true, user_dst, &st, sizeof(st)) < 0)
        return -1;
    return 0;
}