#include "mod.h"

/* 从path中提取第一个元素(文件名)
    例如: path = "/a/b/c" -> name = "a", return "/b/c"
    例如: path = "a/b/c" -> name = "a", return "b/c"
    例如: path = "a" -> name = "a", return ""
    例如: path = "" -> name = "", return 0
*/
static char* get_element(char *path, char *name)
{
    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    
    char *s = path;
    while (*path != '/' && *path != 0)
        path++;
    
    int len = path - s;
    if (len >= MAXLEN_FILENAME)
        len = MAXLEN_FILENAME - 1;
    
    memmove(name, s, len);
    name[len] = 0;
    
    while (*path == '/')
        path++;
    return path;
}

/*
    在目录ip中查找名为name的文件/目录
    找到返回inode_num, 找不到返回INVALID_INODE_NUM
*/
uint32 dentry_search(inode_t *ip, char *name)
{
    // 目录项只存在于目录文件中
    if (ip->disk_info.type != INODE_TYPE_DIR)
        return INVALID_INODE_NUM;
    
    dentry_t de;
    uint32 off = 0;
    
    // 循环读取目录中的所有dentry
    while (off < ip->disk_info.size) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de))
            break; // 读取失败
            
        if (de.name[0] != 0 && strcmp(de.name, name) == 0)
            return de.inode_num;
            
        off += sizeof(de);
    }
    return INVALID_INODE_NUM;
}

/*
    在目录ip中创建一对 (name, inode_num)
    成功返回offset, 失败返回-1
*/
uint32 dentry_create(inode_t *ip, uint32 inode_num, char *name)
{
    if (ip->disk_info.type != INODE_TYPE_DIR)
        return -1;
        
    dentry_t de;
    uint32 off = 0;
    uint32 empty_off = -1; // 记录找到的第一个空槽位
    
    // 1. 遍历目录：检查重名，同时寻找空闲槽位
    while (off < ip->disk_info.size) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de))
            break;
            
        if (de.name[0] == 0) {
            // 记录遇到的第一个空位（优先复用被删除的位置）
            if (empty_off == -1) empty_off = off;
        } else if (strcmp(de.name, name) == 0) {
            return -1; // 已经存在同名文件
        }
        off += sizeof(de);
    }
    
    // 2. 准备新的dentry
    memset(&de, 0, sizeof(de));
    // 使用 strncpy 防止溢出，预留一位给 \0
    strncpy(de.name, name, MAXLEN_FILENAME - 1);
    de.inode_num = inode_num;
    
    // 3. 确定写入位置：如果有空槽位就复用，否则追加到文件末尾
    uint32 write_off;
    if (empty_off != -1) {
        write_off = empty_off;
    } else {
        write_off = ip->disk_info.size;
    }
    
    if (inode_write_data(ip, write_off, sizeof(de), &de, false) != sizeof(de))
        return -1;
        
    return write_off;
}

/*
    从目录ip中删除名为name的项
    返回被删除项的inode_num
*/
uint32 dentry_delete(inode_t *ip, char *name)
{
    if (ip->disk_info.type != INODE_TYPE_DIR)
        return INVALID_INODE_NUM;

    dentry_t de;
    uint32 off = 0;
    
    while (off < ip->disk_info.size) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de))
            break;
            
        if (de.name[0] != 0 && strcmp(de.name, name) == 0) {
            uint32 inum = de.inode_num;
            
            // 清空该项 (name[0] = 0 即标记为无效)
            memset(&de, 0, sizeof(de));
            if (inode_write_data(ip, off, sizeof(de), &de, false) != sizeof(de))
                return INVALID_INODE_NUM;
                
            return inum;
        }
        off += sizeof(de);
    }
    return INVALID_INODE_NUM;
}

/*
    打印目录内容 (Debug用)
*/
void dentry_print(inode_t *ip)
{
    if (ip->disk_info.type != INODE_TYPE_DIR) return;
    
    dentry_t de;
    uint32 off = 0;
    
    printf("Directory content (inode %d):\n", ip->inode_num);
    while (off < ip->disk_info.size) {
        if (inode_read_data(ip, off, sizeof(de), &de, false) != sizeof(de))
            break;
        if (de.name[0] != 0) {
            printf("  name: %s, inode: %d\n", de.name, de.inode_num);
        }
        off += sizeof(de);
    }
}

/*
    解析路径，返回目标inode
    注意: 返回的inode未上锁，引用计数已+1
*/
inode_t* path_to_inode(char *path)
{
    char name[MAXLEN_FILENAME];
    inode_t *ip, *next_ip;
    
    // Lab8 只支持绝对路径
    if(*path == '/')
        ip = inode_get(ROOT_INODE);
    else
        return NULL; 

    // 逐级解析
    while((path = get_element(path, name)) != 0){
        inode_lock(ip);
        if(ip->disk_info.type != INODE_TYPE_DIR){
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }
        
        uint32 next_inum = dentry_search(ip, name);
        inode_unlock(ip);
        
        if(next_inum == INVALID_INODE_NUM){
            inode_put(ip); // 没找到，释放当前目录
            return NULL;
        }
        
        next_ip = inode_get(next_inum);
        inode_put(ip); // 释放父目录，持有子目录
        ip = next_ip;
    }
    return ip;
}

/*
    解析路径，返回父目录inode，并将最后一个元素名填入name
    例如: path="/a/b/c" -> 返回 b_inode, name="c"
*/
/* src/kernel/fs/dentry.c */

inode_t* path_to_parent_inode(char *path, char *name)
{
    inode_t *ip, *next_ip;
    
    if(*path == '/')
        ip = inode_get(ROOT_INODE);
    else
        return NULL;

    path = get_element(path, name);
    
    while(path != 0){
        // 预读下一个元素
        char temp_name[MAXLEN_FILENAME];
        char *rest = get_element(path, temp_name);
        
        // [修改点]：检查 rest 是否为 0，而不是检查 temp_name
        // 如果 rest 为 0，说明 get_element 没有找到下一个元素，路径已结束
        if (rest == 0) {
            // 如果下一个元素为空，说明当前 name 已经是最后一个了
            // 此时 ip 正是 name 的父目录
            return ip;
        }
        
        // 否则继续向下走
        inode_lock(ip);
        if(ip->disk_info.type != INODE_TYPE_DIR){
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }
        
        uint32 next_inum = dentry_search(ip, name);
        inode_unlock(ip);
        
        if(next_inum == INVALID_INODE_NUM){
            inode_put(ip);
            return NULL;
        }
        
        next_ip = inode_get(next_inum);
        inode_put(ip);
        ip = next_ip;
        
        // 更新 loop 变量
        strcpy(name, temp_name);
        path = rest;
    }
    
    return ip;
}