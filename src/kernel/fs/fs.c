#include "mod.h"

#define O_CREATE    FILE_OPEN_CREATE
#define O_RDONLY    FILE_OPEN_READ
#define O_WRONLY    FILE_OPEN_WRITE
#define O_RDWR      (FILE_OPEN_READ | FILE_OPEN_WRITE)
#define O_APPEND    0x08 // 假设的 APPEND 标志，如果 type.h 未定义可暂时设为 0 或自定义
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

/**
 * 第1步：初始化文件系统顶层抽象
 */
void file_init() {
    init_spinlock(&lk_file_table, "file_table");
    acquire_spinlock(&lk_file_table);
    for (int i = 0; i < N_FILE; i++) {
        file_table[i].ref = 0;
    }
    release_spinlock(&lk_file_table);
}

/**
 * 初始化整个文件系统模块
 * 在 main.c 中被调用
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
    device_init();  // 设备文件初始化 (创建 /dev/stdin 等)

    printf("fs_init: done. Magic 0x%x\n", sb.magic_num);
}

/**
 * 从全局文件表中分配一个空闲的 file 结构
 */
file_t* file_alloc() {
    acquire_spinlock(&lk_file_table);
    for (int i = 0; i < N_FILE; i++) {
        if (file_table[i].ref == 0) {
            file_table[i].ref = 1;
            file_table[i].offset = 0;
            file_table[i].readable = false;
            file_table[i].writable = false;
            file_table[i].ip = NULL;
            release_spinlock(&lk_file_table);
            return &file_table[i];
        }
    }
    release_spinlock(&lk_file_table);
    return NULL;
}

/**
 * 增加文件的引用计数
 */
file_t* file_dup(file_t* f) {
    acquire_spinlock(&lk_file_table);
    if (f->ref < 1) panic("file_dup");
    f->ref++;
    release_spinlock(&lk_file_table);
    return f;
}

/**
 * 关闭文件：减少引用计数，若为0则释放关联的 inode
 */
void file_close(file_t *f) {
    acquire_spinlock(&lk_file_table);
    if (f->ref < 1) panic("file_close");
    if (--f->ref > 0) {
        release_spinlock(&lk_file_table);
        return;
    }
    
    // 引用归零，清理资源
    inode_t *ip = f->ip;
    f->ref = 0;
    f->ip = NULL;
    release_spinlock(&lk_file_table);

    if (ip) {
        inode_put(ip);
    }
}

/**
 * 打开文件：将路径解析为 inode 并封装进 file 结构
 */
file_t* file_open(char *path, uint32 open_mode) {
    inode_t *ip;

    if (open_mode & O_CREATE) {
        // 创建模式：在 dentry.c 中实现
        ip = path_create_inode(path, INODE_FILE, 0, 0);
    } else {
        // 查找模式：在 dentry.c 中实现 (支持相对路径)
        ip = __path_to_inode(NULL, path, false);
    }

    if (ip == NULL) return NULL;

    acquire_sleeplock(&ip->slk);

    // 权限检查：目录不能以写方式打开
    if (ip->disk_info.type == INODE_DIRECTORY && (open_mode != O_RDONLY)) {
        release_sleeplock(&ip->slk);
        inode_put(ip);
        return NULL;
    }

    // 设备文件特有的打开检查
    if (ip->disk_info.type == INODE_DEVICE) {
        if (!device_open_check(ip->disk_info.major, open_mode)) {
            release_sleeplock(&ip->slk);
            inode_put(ip);
            return NULL;
        }
    }

    file_t *f = file_alloc();
    if (f == NULL) {
        release_sleeplock(&ip->slk);
        inode_put(ip);
        return NULL;
    }

    f->ip = ip;
    f->readable = (open_mode & O_WRONLY) ? false : true;
    f->writable = (open_mode & O_WRONLY) || (open_mode & O_RDWR);
    
    // 处理追加模式
    if ((open_mode & O_APPEND) && ip->disk_info.type == INODE_FILE) {
        f->offset = ip->disk_info.size;
    } else {
        f->offset = 0;
    }

    release_sleeplock(&ip->slk);
    return f;
}

/**
 * 读取文件：根据文件类型分发到设备读或 inode 读
 */
uint32 file_read(file_t* f, uint32 len, uint64 dst, bool is_user_dst) {
    if (!f->readable) return -1;

    uint32 bytes = 0;
    if (f->ip->disk_info.type == INODE_DEVICE) {
        // 分发到设备驱动 (device.c)
        bytes = device_read_data(f->ip->disk_info.major, len, dst, is_user_dst);
    } else {
        // 普通文件或目录读
        acquire_sleeplock(&f->ip->slk);
        bytes = inode_read(f->ip, dst, f->offset, len, is_user_dst);
        if (bytes > 0) {
            f->offset += bytes;
        }
        release_sleeplock(&f->ip->slk);
    }
    return bytes;
}

/**
 * 写入文件：根据文件类型分发到设备写或 inode 写
 */
uint32 file_write(file_t* f, uint32 len, uint64 src, bool is_user_src) {
    if (!f->writable) return -1;

    uint32 bytes = 0;
    if (f->ip->disk_info.type == INODE_DEVICE) {
        // 分发到设备驱动 (device.c)
        bytes = device_write_data(f->ip->disk_info.major, len, src, is_user_src);
    } else {
        // 普通文件写
        acquire_sleeplock(&f->ip->slk);
        bytes = inode_write(f->ip, src, f->offset, len, is_user_src);
        if (bytes > 0) {
            f->offset += bytes;
        }
        release_sleeplock(&f->ip->slk);
    }
    return bytes;
}

/**
 * 移动读写指针
 */
uint32 file_lseek(file_t *f, uint32 lseek_offset, uint32 lseek_flag) {
    acquire_sleeplock(&f->ip->slk);
    uint32 new_off = f->offset;

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
            release_sleeplock(&f->ip->slk);
            return -1;
    }

    // 简单检查：offset 不能为负（uint32 保证了非负，但需要逻辑合理）
    f->offset = new_off;
    release_sleeplock(&f->ip->slk);
    return new_off;
}

/**
 * 获取文件状态信息 (stat)
 */
uint32 file_get_stat(file_t* f, uint64 user_dst) {
    stat_t st;
    acquire_sleeplock(&f->ip->slk);
    st.inum = f->ip->inum;
    st.type = f->ip->disk_info.type;
    st.nlink = f->ip->nlink;
    st.size = f->ip->disk_info.size;
    release_sleeplock(&f->ip->slk);
    
    // 拷贝到用户态缓冲区
    if (either_copy_to(true, user_dst, &st, sizeof(st)) < 0)
        return -1;
    return 0;
}