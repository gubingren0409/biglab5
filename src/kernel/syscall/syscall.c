#include "mod.h"

// 跳转表: 系统调用号 -> 系统调用服务函数
static uint64 (*syscalls[])(void) = {
    [SYS_brk] sys_brk,
    [SYS_mmap] sys_mmap,
    [SYS_munmap] sys_munmap,
    [SYS_print_str] sys_print_str,
    [SYS_print_int] sys_print_int,
    [SYS_getpid] sys_getpid,
    [SYS_fork] sys_fork,
    [SYS_wait] sys_wait,
    [SYS_exit] sys_exit,
    [SYS_sleep] sys_sleep,
    [SYS_alloc_block] sys_alloc_block,
    [SYS_free_block] sys_free_block,
    [SYS_alloc_inode] sys_alloc_inode,
    [SYS_free_inode] sys_free_inode,
    [SYS_show_bitmap] sys_show_bitmap,
    [SYS_get_block] sys_get_block,
    [SYS_read_block] sys_read_block,
    [SYS_write_block] sys_write_block,
    [SYS_put_block] sys_put_block,
    [SYS_show_buffer] sys_show_buffer,
    [SYS_flush_buffer] sys_flush_buffer,
    // [Lab 9] 新增文件系统调用
    [SYS_open] sys_open,
    [SYS_close] sys_close,
    [SYS_read] sys_read,
    [SYS_write] sys_write,
    [SYS_lseek] sys_lseek,
    [SYS_dup] sys_dup,
    [SYS_fstat] sys_fstat,
    [SYS_get_dentries] sys_get_dentries,
    [SYS_mkdir] sys_mkdir,
    [SYS_chdir] sys_chdir,
    [SYS_print_cwd] sys_print_cwd,
    [SYS_link] sys_link,
    [SYS_unlink] sys_unlink,
    [SYS_exec] sys_exec,
};

// 基于系统调用表的请求跳转
void syscall()
{
    proc_t *p = myproc();

    int sys_num = p->tf->a7;
    if (sys_num < 0 || sys_num > SYS_MAX_NUM || syscalls[sys_num] == NULL) {
        printf("unknown syscall %d from pid = %d\n", sys_num, p->pid);
        panic("syscall");
    } else {
        p->tf->a0 = syscalls[sys_num]();
    }
}

// 读取 n 号参数,它放在 an 寄存器中
static uint64 arg_raw(int n)
{
    proc_t *proc = myproc();
    switch (n) {
        case 0: return proc->tf->a0;
        case 1: return proc->tf->a1;
        case 2: return proc->tf->a2;
        case 3: return proc->tf->a3;
        case 4: return proc->tf->a4;
        case 5: return proc->tf->a5;
        default: panic("arg_raw: illegal arg num"); return -1;
    }
}

// 读取 n 号参数, 作为 uint32 存储
void arg_uint32(int n, uint32 *ip) { *ip = arg_raw(n); }

// 读取 n 号参数, 作为 uint64 存储
void arg_uint64(int n, uint64 *ip) { *ip = arg_raw(n); }

// [修复] 返回类型改为 int
// 读取 n 号参数指向的字符串到 buf
int arg_str(int n, char *buf, int maxlen) {
    proc_t *p = myproc();
    uint64 addr;
    arg_uint64(n, &addr);
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, maxlen);
    return 0;
}

// 兼容 wrapper，供 sysfunc.c 使用
int arg_int(int n, int *ip) {
    *ip = arg_raw(n);
    return 0;
}

int arg_addr(int n, uint64 *ip) {
    *ip = arg_raw(n);
    return 0;
}