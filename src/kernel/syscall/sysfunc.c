#include "mod.h"

#define MAX_PATH 128
#define MAX_ARG  32

// -------------------------------------------------------------------
// Lab 1-4: 基础/测试系统调用 (补充缺失部分)
// -------------------------------------------------------------------

uint64 sys_print_str() {
    char buf[STR_MAXLEN];
    if (arg_str(0, buf, STR_MAXLEN) < 0) return -1;
    printf("%s", buf);
    return 0;
}

uint64 sys_print_int() {
    int val;
    arg_int(0, &val);
    printf("%d", val);
    return 0;
}

// -------------------------------------------------------------------
// Lab 5-6: 进程与内存系统调用
// -------------------------------------------------------------------

uint64 sys_brk(void) {
    uint64 addr;
    if (arg_addr(0, &addr) < 0) return -1;
    proc_t *p = myproc();
    uint64 old_heap = p->heap_top;
    
    if (addr > old_heap) {
        uint32 len = addr - old_heap;
        p->heap_top = uvm_heap_grow(p->pgtbl, old_heap, len);
    } else if (addr < old_heap && addr >= USER_BASE + PGSIZE) {
        uint32 len = old_heap - addr;
        p->heap_top = uvm_heap_ungrow(p->pgtbl, old_heap, len);
    }
    return p->heap_top;
}

uint64 sys_mmap(void) {
    uint64 addr;
    uint32 len, prot, flags;
    if (arg_addr(0, &addr) < 0 || arg_int(1, (int*)&len) < 0 || 
        arg_int(2, (int*)&prot) < 0 || arg_int(3, (int*)&flags) < 0) return -1;
    
    uint32 npages = (len + PGSIZE - 1) / PGSIZE;
    uvm_mmap(addr, npages, prot);
    return addr; 
}

uint64 sys_munmap(void) {
    uint64 addr;
    uint32 len;
    if (arg_addr(0, &addr) < 0 || arg_int(1, (int*)&len) < 0) return -1;
    
    uint32 npages = (len + PGSIZE - 1) / PGSIZE;
    uvm_munmap(addr, npages);
    return 0;
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
    return 0; 
}

uint64 sys_getpid(void) {
    return myproc()->pid;
}

uint64 sys_sleep(void) {
    int n;
    if (arg_int(0, &n) < 0) return -1;
    
    uint64 start = timer_get_ticks();
    while (timer_get_ticks() - start < n) {
        proc_yield();
    }
    return 0;
}

// -------------------------------------------------------------------
// Lab 7-8: 文件系统测试调用 (补充缺失部分)
// -------------------------------------------------------------------

uint64 sys_alloc_block() { return bitmap_alloc_block(); }

uint64 sys_free_block() {
    int block_num;
    arg_int(0, &block_num);
    bitmap_free_block(block_num);
    return 0;
}

uint64 sys_alloc_inode() { return bitmap_alloc_inode(); }

uint64 sys_free_inode() {
    int inode_num;
    arg_int(0, &inode_num);
    bitmap_free_inode(inode_num);
    return 0;
}

uint64 sys_show_bitmap() {
    int print_data;
    arg_int(0, &print_data);
    bitmap_print(print_data);
    return 0;
}

// 假设这些是测试缓冲区功能的系统调用
// 返回 buffer 对象的内核地址 (测试用)
uint64 sys_get_block() {
    int block_num;
    arg_int(0, &block_num);
    return (uint64)buffer_get(block_num);
}

uint64 sys_read_block() {
    int block_num; uint64 user_dst;
    arg_int(0, &block_num);
    arg_addr(1, &user_dst);
    
    buffer_t *b = buffer_get(block_num);
    // 拷贝数据到用户空间
    uvm_copyout(myproc()->pgtbl, user_dst, (uint64)b->data, BLOCK_SIZE);
    buffer_put(b);
    return 0;
}

uint64 sys_write_block() {
    int block_num; uint64 user_src;
    arg_int(0, &block_num);
    arg_addr(1, &user_src);
    
    buffer_t *b = buffer_get(block_num);
    // 从用户空间拷贝数据
    uvm_copyin(myproc()->pgtbl, (uint64)b->data, user_src, BLOCK_SIZE);
    buffer_write(b); // 标记写回
    buffer_put(b);
    return 0;
}

uint64 sys_put_block() {
    uint64 buf_addr;
    arg_addr(0, &buf_addr);
    buffer_put((buffer_t*)buf_addr);
    return 0;
}

uint64 sys_show_buffer() {
    buffer_print_info();
    return 0;
}

uint64 sys_flush_buffer() {
    int count;
    arg_int(0, &count);
    return buffer_freemem(count);
}


// -------------------------------------------------------------------
// Lab 9: 文件系统调用
// -------------------------------------------------------------------

uint64 sys_exec(void) {
    char path[MAX_PATH];
    uint64 argv_ptr;
    char *argv[MAX_ARG];
    
    if (arg_str(0, path, MAX_PATH) < 0 || arg_addr(1, &argv_ptr) < 0) return -1;

    for (int i = 0; i < MAX_ARG; i++) {
        uint64 u_arg;
        uvm_copyin(myproc()->pgtbl, (uint64)&u_arg, argv_ptr + i * sizeof(uint64), sizeof(uint64));
        
        if (u_arg == 0) {
            argv[i] = 0;
            break;
        }
        argv[i] = (char*)pmem_alloc(false);
        uvm_copyin_str(myproc()->pgtbl, (uint64)argv[i], u_arg, PGSIZE);
    }

    int ret = proc_exec(path, argv);

    for (int i = 0; i < MAX_ARG && argv[i] != 0; i++) {
        pmem_free((uint64)argv[i], false);
    }

    return ret;
}

uint64 sys_open(void) {
    char path[MAX_PATH];
    int mode;
    if (arg_str(0, path, MAX_PATH) < 0 || arg_int(1, &mode) < 0) return -1;

    file_t *f = file_open(path, mode);
    if (f == NULL) return -1;

    proc_t *p = myproc();
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
    
    proc_t *p = myproc();
    if (p->open_file[fd] == NULL) return -1;
    
    file_close(p->open_file[fd]);
    p->open_file[fd] = NULL;
    return 0;
}

uint64 sys_read(void) {
    int fd, len;
    uint64 buf;
    if (arg_int(0, &fd) < 0 || arg_int(1, &len) < 0 || arg_addr(2, &buf) < 0) return -1;
    if (fd < 0 || fd >= N_OPEN_FILE || myproc()->open_file[fd] == NULL) return -1;
    
    return file_read(myproc()->open_file[fd], (uint32)len, buf, true);
}

uint64 sys_write(void) {
    int fd, len;
    uint64 buf;
    if (arg_int(0, &fd) < 0 || arg_int(1, &len) < 0 || arg_addr(2, &buf) < 0) return -1;
    if (fd < 0 || fd >= N_OPEN_FILE || myproc()->open_file[fd] == NULL) return -1;
    
    return file_write(myproc()->open_file[fd], (uint32)len, buf, true);
}

uint64 sys_lseek(void) {
    int fd, offset, flag;
    if (arg_int(0, &fd) < 0 || arg_int(1, &offset) < 0 || arg_int(2, &flag) < 0) return -1;
    if (fd < 0 || fd >= N_OPEN_FILE || myproc()->open_file[fd] == NULL) return -1;
    
    return file_lseek(myproc()->open_file[fd], (uint32)offset, (uint32)flag);
}

uint64 sys_dup(void) {
    int fd;
    if (arg_int(0, &fd) < 0 || fd < 0 || fd >= N_OPEN_FILE) return -1;
    
    proc_t *p = myproc();
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
    if (fd < 0 || fd >= N_OPEN_FILE || myproc()->open_file[fd] == NULL) return -1;
    
    return file_get_stat(myproc()->open_file[fd], addr);
}

uint64 sys_get_dentries(void) {
    int fd, len;
    uint64 addr;
    if (arg_int(0, &fd) < 0 || arg_addr(1, &addr) < 0 || arg_int(2, &len) < 0) return -1;
    
    file_t *f = myproc()->open_file[fd];
    // [修复] 使用 INODE_TYPE_DIR
    if (f == NULL || f->ip->disk_info.type != INODE_TYPE_DIR) return -1;
    
    return dentry_transmit(f->ip, addr, (uint32)len, true);
}

uint64 sys_mkdir(void) {
    char path[MAX_PATH];
    if (arg_str(0, path, MAX_PATH) < 0) return -1;
    
    // [修复] 使用 INODE_TYPE_DIR
    inode_t *ip = path_create_inode(path, INODE_TYPE_DIR, 0, 0);
    if (ip == NULL) return -1;
    
    inode_put(ip);
    return 0;
}

uint64 sys_chdir(void) {
    char path[MAX_PATH];
    if (arg_str(0, path, MAX_PATH) < 0) return -1;
    
    inode_t *ip = __path_to_inode(NULL, path, false);
    // [修复] 使用 INODE_TYPE_DIR
    if (ip == NULL || ip->disk_info.type != INODE_TYPE_DIR) {
        if (ip) inode_put(ip);
        return -1;
    }
    
    proc_t *p = myproc();
    inode_put(p->cwd);
    p->cwd = ip; 
    return 0;
}

uint64 sys_print_cwd(void) {
    char path[MAX_PATH];
    proc_t *p = myproc();
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