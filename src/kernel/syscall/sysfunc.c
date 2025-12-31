#include "mod.h"

// --- 进程与内存相关系统调用 (Lab 5 & 6) ---

uint64 sys_brk(void) {
    uint64 addr;
    if (arg_addr(0, &addr) < 0) return -1;
    proc_t *p = curr_proc();
    uint64 old_heap = p->heap_top;
    if (addr > old_heap) {
        p->heap_top = uvm_heap_grow(p->pgtbl, old_heap, addr, PTE_R | PTE_W);
    } else if (addr < old_heap && addr >= USER_BASE + PGSIZE) {
        p->heap_top = uvm_heap_shrink(p->pgtbl, old_heap, addr);
    }
    return p->heap_top;
}

uint64 sys_mmap(void) {
    uint64 addr;
    uint32 len, prot, flags;
    if (arg_addr(0, &addr) < 0 || arg_int(1, (int*)&len) < 0 || 
        arg_int(2, (int*)&prot) < 0 || arg_int(3, (int*)&flags) < 0) return -1;
    return (uint64)mmap_region_create(curr_proc(), addr, len, prot, flags);
}

uint64 sys_munmap(void) {
    uint64 addr;
    uint32 len;
    if (arg_addr(0, &addr) < 0 || arg_int(1, (int*)&len) < 0) return -1;
    return mmap_region_delete(curr_proc(), addr, len);
}

uint64 sys_fork(void) {
    return proc_fork();
}

uint64 sys_wait(void) {
    uint64 addr;
    if (arg_addr(0, &addr) < 0) return -1;
    return proc_wait(addr);
}

uint64 sys_exit(void) {
    int code;
    if (arg_int(0, &code) < 0) return -1;
    proc_exit(code);
    return 0; // 不会执行到这里
}

uint64 sys_getpid(void) {
    return curr_proc()->pid;
}

uint64 sys_sleep(void) {
    int n;
    if (arg_int(0, &n) < 0) return -1;
    // 简化实现：使用 ticks 或循环
    uint32 start = get_ticks();
    while (get_ticks() - start < n) {
        proc_yield();
    }
    return 0;
}

// --- 文件系统相关系统调用 (Lab 9 NEW) ---

uint64 sys_exec(void) {
    char path[MAX_PATH];
    uint64 argv_ptr;
    char *argv[MAX_ARG];
    
    if (arg_str(0, path, MAX_PATH) < 0 || arg_addr(1, &argv_ptr) < 0) return -1;

    // 获取参数列表
    for (int i = 0; i < MAX_ARG; i++) {
        uint64 u_arg;
        if (uvm_copyin(curr_proc()->pgtbl, (uint64)&u_arg, argv_ptr + i * sizeof(uint64), sizeof(uint64)) < 0)
            return -1;
        if (u_arg == 0) {
            argv[i] = 0;
            break;
        }
        argv[i] = (char*)pmem_alloc(false);
        if (uvm_copyinstr(curr_proc()->pgtbl, argv[i], u_arg, PGSIZE) < 0)
            return -1;
    }

    int ret = proc_exec(path, argv);

    // 释放内核中临时分配的参数内存
    for (int i = 0; i < MAX_ARG && argv[i] != 0; i++) {
        pmem_free(argv[i]);
    }

    return ret;
}

uint64 sys_open(void) {
    char path[MAX_PATH];
    int mode;
    if (arg_str(0, path, MAX_PATH) < 0 || arg_int(1, &mode) < 0) return -1;

    file_t *f = file_open(path, mode);
    if (f == NULL) return -1;

    proc_t *p = curr_proc();
    for (int i = 0; i < N_OPEN_FILE; i++) {
        if (p->open_file[i] == NULL) {
            p->open_file[i] = f;
            return i;
        }
    }
    file_close(f);
    return -1;
}

uint64 sys_close(void) {
    int fd;
    if (arg_int(0, &fd) < 0 || fd < 0 || fd >= N_OPEN_FILE) return -1;
    
    proc_t *p = curr_proc();
    if (p->open_file[fd] == NULL) return -1;
    
    file_close(p->open_file[fd]);
    p->open_file[fd] = NULL;
    return 0;
}

uint64 sys_read(void) {
    int fd, len;
    uint64 buf;
    if (arg_int(0, &fd) < 0 || arg_int(1, &len) < 0 || arg_addr(2, &buf) < 0) return -1;
    if (fd < 0 || fd >= N_OPEN_FILE || curr_proc()->open_file[fd] == NULL) return -1;
    
    return file_read(curr_proc()->open_file[fd], (uint32)len, buf, true);
}

uint64 sys_write(void) {
    int fd, len;
    uint64 buf;
    if (arg_int(0, &fd) < 0 || arg_int(1, &len) < 0 || arg_addr(2, &buf) < 0) return -1;
    if (fd < 0 || fd >= N_OPEN_FILE || curr_proc()->open_file[fd] == NULL) return -1;
    
    return file_write(curr_proc()->open_file[fd], (uint32)len, buf, true);
}

uint64 sys_lseek(void) {
    int fd, offset, flag;
    if (arg_int(0, &fd) < 0 || arg_int(1, &offset) < 0 || arg_int(2, &flag) < 0) return -1;
    if (fd < 0 || fd >= N_OPEN_FILE || curr_proc()->open_file[fd] == NULL) return -1;
    
    return file_lseek(curr_proc()->open_file[fd], (uint32)offset, (uint32)flag);
}

uint64 sys_dup(void) {
    int fd;
    if (arg_int(0, &fd) < 0 || fd < 0 || fd >= N_OPEN_FILE) return -1;
    
    proc_t *p = curr_proc();
    if (p->open_file[fd] == NULL) return -1;
    
    for (int i = 0; i < N_OPEN_FILE; i++) {
        if (p->open_file[i] == NULL) {
            p->open_file[i] = file_dup(p->open_file[fd]);
            return i;
        }
    }
    return -1;
}

uint64 sys_fstat(void) {
    int fd;
    uint64 addr;
    if (arg_int(0, &fd) < 0 || arg_addr(1, &addr) < 0) return -1;
    if (fd < 0 || fd >= N_OPEN_FILE || curr_proc()->open_file[fd] == NULL) return -1;
    
    return file_get_stat(curr_proc()->open_file[fd], addr);
}

uint64 sys_get_dentries(void) {
    int fd, len;
    uint64 addr;
    if (arg_int(0, &fd) < 0 || arg_addr(1, &addr) < 0 || arg_int(2, &len) < 0) return -1;
    
    file_t *f = curr_proc()->open_file[fd];
    if (f == NULL || f->ip->type != INODE_DIRECTORY) return -1;
    
    return dentry_transmit(f->ip, addr, (uint32)len, true);
}

uint64 sys_mkdir(void) {
    char path[MAX_PATH];
    if (arg_str(0, path, MAX_PATH) < 0) return -1;
    
    inode_t *ip = path_create_inode(path, INODE_DIRECTORY, 0, 0);
    if (ip == NULL) return -1;
    
    inode_put(ip);
    return 0;
}

uint64 sys_chdir(void) {
    char path[MAX_PATH];
    if (arg_str(0, path, MAX_PATH) < 0) return -1;
    
    inode_t *ip = __path_to_inode(NULL, path, false);
    if (ip == NULL || ip->type != INODE_DIRECTORY) {
        if (ip) inode_put(ip);
        return -1;
    }
    
    proc_t *p = curr_proc();
    inode_put(p->cwd);
    p->cwd = ip; // 切换工作目录
    return 0;
}

uint64 sys_print_cwd(void) {
    char path[MAX_PATH];
    proc_t *p = curr_proc();
    if (inode_to_path(p->cwd, path, MAX_PATH) < 0) return -1;
    printf("%s\n", path);
    return 0;
}

uint64 sys_link(void) {
    char old_path[MAX_PATH], new_path[MAX_PATH];
    if (arg_str(0, old_path, MAX_PATH) < 0 || arg_str(1, new_path, MAX_PATH) < 0) return -1;
    return path_link(old_path, new_path);
}

uint64 sys_unlink(void) {
    char path[MAX_PATH];
    if (arg_str(0, path, MAX_PATH) < 0) return -1;
    return path_unlink(path);
}