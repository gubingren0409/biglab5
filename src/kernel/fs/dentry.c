#include "mod.h"
#include "../lib/method.h"
#include "../proc/mod.h"
#include "../mem/method.h"

// 映射常量与内部定义
#define N_NAME          MAXLEN_FILENAME
#define MAX_PATH        128
#define INODE_DIRECTORY INODE_TYPE_DIR
#define ROOT_INUM       ROOT_INODE

// 1. 手动实现 strcpy，避免 implicit declaration
static char* strcpy(char *s, const char *t) {
    char *os = s;
    while ((*s++ = *t++) != 0);
    return os;
}

// 2. 手动实现 either_copy_to
// 这是一个辅助函数，根据目标是在用户态还是内核态选择拷贝方式
static int either_copy_to(bool is_user, uint64 dst, void *src, uint32 len) {
    if (is_user) {
        uvm_copyout(myproc()->pgtbl, dst, (uint64)src, len);
    } else {
        memmove((void*)dst, src, len);
    }
    return 0;
}

// 提取路径中的文件名
static char* get_element(char *path, char *name) {
    while (*path == '/') path++;
    if (*path == 0) return 0;
    
    char *s = path;
    while (*path != '/' && *path != 0) path++;
    
    int len = path - s;
    if (len >= MAXLEN_FILENAME) len = MAXLEN_FILENAME - 1;
    
    memmove(name, s, len);
    name[len] = 0;
    
    while (*path == '/') path++;
    return path;
}

// --- 基础目录操作 ---

uint32 dentry_search(inode_t *ip, char *name) {
    if (ip->disk_info.type != INODE_TYPE_DIR) return INVALID_INODE_NUM;
    
    dentry_t de;
    uint32 off = 0;
    
    while (off < ip->disk_info.size) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de)) break;
        if (de.name[0] != 0 && strncmp(de.name, name, MAXLEN_FILENAME) == 0) return de.inode_num;
        off += sizeof(de);
    }
    return INVALID_INODE_NUM;
}

uint32 dentry_create(inode_t *ip, uint32 inode_num, char *name) {
    if (ip->disk_info.type != INODE_TYPE_DIR) return -1;
    
    dentry_t de;
    uint32 off = 0;
    uint32 empty_off = -1;
    
    while (off < ip->disk_info.size) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de)) break;
        if (de.name[0] == 0) {
            if (empty_off == -1) empty_off = off;
        } else if (strncmp(de.name, name, MAXLEN_FILENAME) == 0) {
            return -1;
        }
        off += sizeof(de);
    }
    
    memset(&de, 0, sizeof(de));
    memmove(de.name, name, MAXLEN_FILENAME - 1);
    de.inode_num = inode_num;
    
    uint32 write_off = (empty_off != -1) ? empty_off : ip->disk_info.size;
    if (inode_write_data(ip, write_off, sizeof(de), &de, false) != sizeof(de)) return -1;
    
    return write_off;
}

uint32 dentry_delete(inode_t *ip, char *name) {
    if (ip->disk_info.type != INODE_TYPE_DIR) return INVALID_INODE_NUM;

    dentry_t de;
    uint32 off = 0;
    
    while (off < ip->disk_info.size) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de)) break;
        if (de.name[0] != 0 && strncmp(de.name, name, MAXLEN_FILENAME) == 0) {
            uint32 inum = de.inode_num;
            memset(&de, 0, sizeof(de));
            if (inode_write_data(ip, off, sizeof(de), &de, false) != sizeof(de)) return INVALID_INODE_NUM;
            return inum;
        }
        off += sizeof(de);
    }
    return INVALID_INODE_NUM;
}

void dentry_print(inode_t *ip) {
    if (ip->disk_info.type != INODE_TYPE_DIR) return;
    dentry_t de;
    uint32 off = 0;
    printf("Directory content (inode %d):\n", ip->inode_num);
    while (off < ip->disk_info.size) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de)) break;
        if (de.name[0] != 0) {
            printf("  name: %s, inode: %d\n", de.name, de.inode_num);
        }
        off += sizeof(de);
    }
}

// --- 路径解析与增强功能 ---

inode_t* path_to_inode(char *path) {
    char name[MAXLEN_FILENAME];
    inode_t *ip, *next_ip;
    
    if (*path == '/') {
        ip = inode_get(ROOT_INODE);
    } else {
        ip = inode_dup(myproc()->cwd);
    }

    if (ip == NULL) return NULL;

    while ((path = get_element(path, name)) != 0) {
        inode_lock(ip);
        if (ip->disk_info.type != INODE_TYPE_DIR) {
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }
        
        uint32 next_inum = dentry_search(ip, name);
        inode_unlock(ip);
        
        if (next_inum == INVALID_INODE_NUM) {
            inode_put(ip);
            return NULL;
        }
        
        next_ip = inode_get(next_inum);
        inode_put(ip);
        ip = next_ip;
    }
    return ip;
}

inode_t* path_to_parent_inode(char *path, char *name) {
    inode_t *ip, *next_ip;
    
    if (*path == '/') {
        ip = inode_get(ROOT_INODE);
    } else {
        ip = inode_dup(myproc()->cwd);
    }

    if (ip == NULL) return NULL;

    path = get_element(path, name);
    
    while (path != 0) {
        char temp_name[MAXLEN_FILENAME];
        char *rest = get_element(path, temp_name);
        
        if (rest == 0) return ip;
        
        inode_lock(ip);
        if (ip->disk_info.type != INODE_TYPE_DIR) {
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }
        
        uint32 next_inum = dentry_search(ip, name);
        inode_unlock(ip);
        
        if (next_inum == INVALID_INODE_NUM) {
            inode_put(ip);
            return NULL;
        }
        
        next_ip = inode_get(next_inum);
        inode_put(ip);
        ip = next_ip;
        
        strcpy(name, temp_name);
        path = rest;
    }
    
    return ip;
}

uint32 dentry_search_2(inode_t *ip, uint32 inode_num, char *name) {
    dentry_t de;
    for (uint32 off = 0; off < ip->disk_info.size; off += sizeof(de)) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de))
            return -1;
        if (de.inode_num == inode_num) {
            memmove(name, de.name, N_NAME);
            return off;
        }
    }
    return -1;
}

uint32 dentry_transmit(inode_t *ip, uint64 dst, uint32 len, bool is_user_dst) {
    dentry_t de;
    uint32 count = 0;
    for (uint32 off = 0; off < ip->disk_info.size && count + sizeof(de) <= len; off += sizeof(de)) {
        inode_read_data(ip, off, sizeof(de), &de, false);
        if (de.inode_num != 0) {
            if (either_copy_to(is_user_dst, dst + count, &de, sizeof(de)) < 0)
                break;
            count += sizeof(de);
        }
    }
    return count;
}

uint32 inode_to_path(inode_t *ip, char *path, uint32 len) {
    if (ip->disk_info.type != INODE_DIRECTORY) return -1;
    char buf[MAX_PATH];
    int pos = MAX_PATH - 1;
    buf[pos] = '\0';
    
    inode_t *curr = inode_dup(ip);
    while (curr->inode_num != ROOT_INUM) {
        inode_t *parent = path_to_inode(".."); 
        if (!parent) { inode_put(curr); return -1; }
        
        char name[N_NAME];
        dentry_search_2(parent, curr->inode_num, name);
        int nlen = strlen(name);
        pos -= nlen;
        memmove(buf + pos, name, nlen);
        pos--;
        buf[pos] = '/';
        
        inode_put(curr);
        curr = parent;
    }
    if (pos == MAX_PATH - 1) buf[--pos] = '/';
    
    uint32 final_len = MAX_PATH - 1 - pos;
    if (final_len >= len) { inode_put(curr); return -1; }
    memmove(path, buf + pos, final_len + 1);
    inode_put(curr);
    return 0;
}

inode_t* path_create_inode(char *path, uint16 type, uint16 major, uint16 minor) {
    char name[N_NAME];
    inode_t *dp = path_to_parent_inode(path, name);
    if (!dp) return NULL;
    
    inode_lock(dp);
    if (dentry_search(dp, name) != INVALID_INODE_NUM) {
        inode_unlock(dp);
        inode_put(dp);
        return NULL;
    }
    
    inode_t *ip = inode_create(type, major, minor);
    if (!ip) { inode_unlock(dp); inode_put(dp); return NULL; }
    
    inode_lock(ip);
    ip->disk_info.nlink = 1;
    inode_rw(ip, true); // 写入磁盘
    
    if (type == INODE_DIRECTORY) {
        dentry_create(ip, ip->inode_num, ".");
        dentry_create(ip, dp->inode_num, "..");
        ip->disk_info.nlink++; // 父目录引用
        inode_rw(ip, true);
    }
    
    dentry_create(dp, ip->inode_num, name);
    inode_unlock(ip);
    inode_unlock(dp);
    inode_put(dp);
    return ip;
}

uint32 path_link(char *old_path, char *new_path) {
    inode_t *ip = path_to_inode(old_path);
    if (!ip) return -1;
    if (ip->disk_info.type == INODE_DIRECTORY) { inode_put(ip); return -1; }
    
    char name[N_NAME];
    inode_t *dp = path_to_parent_inode(new_path, name);
    if (!dp) { inode_put(ip); return -1; }
    
    inode_lock(dp);
    if (dentry_search(dp, name) != INVALID_INODE_NUM) {
        inode_unlock(dp);
        inode_put(dp); inode_put(ip); return -1;
    }
    
    dentry_create(dp, ip->inode_num, name);
    
    inode_lock(ip);
    ip->disk_info.nlink++;
    inode_rw(ip, true);
    inode_unlock(ip);
    
    inode_unlock(dp);
    inode_put(dp);
    inode_put(ip);
    return 0;
}

uint32 path_unlink(char *path) {
    char name[N_NAME];
    inode_t *dp = path_to_parent_inode(path, name);
    if (!dp) return -1;
    
    inode_lock(dp);
    // 注意：不能删除 . 和 ..
    if (strncmp(name, ".", N_NAME) == 0 || strncmp(name, "..", N_NAME) == 0) {
        inode_unlock(dp); inode_put(dp); return -1;
    }
    
    uint32 inum = dentry_delete(dp, name);
    if (inum == INVALID_INODE_NUM) {
        inode_unlock(dp); inode_put(dp); return -1;
    }
    
    inode_t *ip = inode_get(inum);
    inode_lock(ip);
    if (ip->disk_info.nlink > 0) {
        ip->disk_info.nlink--;
        inode_rw(ip, true);
        
        if (ip->disk_info.type == INODE_DIRECTORY) {
            dp->disk_info.nlink--;
            inode_rw(dp, true);
        }
    }
    
    inode_unlock(ip);
    inode_put(ip);
    
    inode_unlock(dp);
    inode_put(dp);
    return 0;
}