#include "mod.h"
#include "../arch/mod.h"
#include "../mem/mod.h"
#include "../lib/mod.h"
#include "../fs/mod.h" // 必须包含文件系统模块

// 引入用户初始化代码
#include "../../user/initcode.h"

// 外部汇编与函数声明
extern char trampoline[];
extern void swtch(context_t *old, context_t *new);
extern void trap_user_return();
extern void fs_init();

// --- 全局控制 ---

// 调度器调试开关：置 1 开启调度日志，置 0 关闭
#define SCHED_TRACE 0

// --- 静态资源管理 ---

// 进程池：所有进程控制块的仓库
static proc_t proc_pool[N_PROC];
// 指向首个用户进程（通常是 init）
static proc_t *init_process;

// PID 分配锁与计数器
static spinlock_t pid_lock;
static int next_pid = 1;

// 进程生命周期锁：用于保护 wait/exit 操作中的进程树关系
static spinlock_t lifecycle_lock;

// --- 内部函数 ---

// 分配一个新的 PID
static int allocate_pid()
{
    int pid;
    spinlock_acquire(&pid_lock);
    pid = next_pid++;
    spinlock_release(&pid_lock);
    return pid;
}

// 进程初次运行的入口函数 (内核态 -> 用户态)
static void proc_entry_point()
{
    // 释放调度器切换过来时持有的进程锁
    spinlock_release(&myproc()->lk);

    // [NEW] 如果是第一个进程(PID=1)，负责初始化文件系统
    if (myproc()->pid == 1) {
        fs_init();
    }

    // 返回用户空间
    trap_user_return();
}

// 进程模块初始化
void proc_init()
{
    spinlock_init(&pid_lock, "pid_allocator");
    spinlock_init(&lifecycle_lock, "proc_lifecycle");
    
    // 初始化进程池
    for (int i = 0; i < N_PROC; i++) {
        spinlock_init(&proc_pool[i].lk, "proc_lock");
        proc_pool[i].state = UNUSED;
        // 预先计算好每个进程的内核栈基址
        proc_pool[i].kstack = KSTACK(i);

        // LAB-9: 初始化文件系统相关字段
        proc_pool[i].cwd = NULL;
        for(int j = 0; j < N_OPEN_FILE; j++) {
            proc_pool[i].open_file[j] = NULL;
        }
    }
}

// 初始化进程页表
pgtbl_t proc_pgtbl_init(uint64 tf_va)
{
    pgtbl_t tbl = (pgtbl_t)pmem_alloc(true);
    if (!tbl) return NULL;
    memset(tbl, 0, PGSIZE);

    vm_mappages(tbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    vm_mappages(tbl, TRAPFRAME, tf_va, PGSIZE, PTE_R | PTE_W);

    return tbl;
}

// 申请一个空闲进程块
proc_t *proc_alloc()
{
    proc_t *p = NULL;

    for (int i = 0; i < N_PROC; i++) {
        spinlock_acquire(&proc_pool[i].lk);
        if (proc_pool[i].state == UNUSED) {
            p = &proc_pool[i];
            break;
        }
        spinlock_release(&proc_pool[i].lk);
    }

    if (!p) return NULL;

    p->pid = allocate_pid();
    p->state = UNUSED; 

    if ((p->tf = (trapframe_t *)pmem_alloc(true)) == NULL) {
        spinlock_release(&p->lk);
        return NULL;
    }
    memset(p->tf, 0, PGSIZE);

    if ((p->pgtbl = proc_pgtbl_init((uint64)p->tf)) == NULL) {
        pmem_free((uint64)p->tf, true);
        p->tf = NULL;
        spinlock_release(&p->lk);
        return NULL;
    }

    memset(&p->ctx, 0, sizeof(p->ctx));
    p->ctx.ra = (uint64)proc_entry_point;
    p->ctx.sp = p->kstack + PGSIZE;

    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->mmap = NULL;
    memset(p->name, 0, sizeof(p->name));

    // LAB-9: 确保分配时清理文件字段
    p->cwd = NULL;
    for(int i = 0; i < N_OPEN_FILE; i++) p->open_file[i] = NULL;

    return p;
}

// 释放进程资源 (调用者需持有 p->lk)
void proc_free(proc_t *p)
{
    // LAB-9: 释放当前工作目录引用
    if(p->cwd) {
        inode_put(p->cwd);
        p->cwd = NULL;
    }

    // LAB-9: 释放所有打开的文件
    for(int i = 0; i < N_OPEN_FILE; i++) {
        if(p->open_file[i]) {
            file_close(p->open_file[i]);
            p->open_file[i] = NULL;
        }
    }

    if (p->tf) pmem_free((uint64)p->tf, true);
    p->tf = NULL;

    if (p->pgtbl) {
        vm_unmappages(p->pgtbl, USER_BASE, PGSIZE, true);
        if (p->heap_top > USER_BASE + PGSIZE)
             vm_unmappages(p->pgtbl, USER_BASE + PGSIZE, p->heap_top - (USER_BASE + PGSIZE), true);
        if (p->ustack_npage > 0)
            vm_unmappages(p->pgtbl, TRAPFRAME - p->ustack_npage * PGSIZE, p->ustack_npage * PGSIZE, true);
        
        mmap_region_t *m = p->mmap;
        while (m) {
            vm_unmappages(p->pgtbl, m->begin, m->npages * PGSIZE, true);
            mmap_region_t *next = m->next;
            mmap_region_free(m);
            m = next;
        }
        p->mmap = NULL;
        
        vm_unmappages(p->pgtbl, TRAMPOLINE, PGSIZE, false);
        vm_unmappages(p->pgtbl, TRAPFRAME, PGSIZE, false);
        pmem_free((uint64)p->pgtbl, true);
    }
    p->pgtbl = NULL;
    p->pid = 0;
    p->parent = NULL;
    p->name[0] = 0;
    p->state = UNUSED;
    
    spinlock_release(&p->lk);
}

// 构建第一个用户进程 (proczero)
void proc_make_first()
{
    proc_t *p = proc_alloc();
    init_process = p;

    void *code_mem = pmem_alloc(false);
    memmove(code_mem, target_user_initcode, target_user_initcode_len);
    vm_mappages(p->pgtbl, USER_BASE, (uint64)code_mem, PGSIZE, PTE_R|PTE_W|PTE_X|PTE_U);

    void *stack_mem = pmem_alloc(false);
    vm_mappages(p->pgtbl, TRAPFRAME - PGSIZE, (uint64)stack_mem, PGSIZE, PTE_R|PTE_W|PTE_U);
    p->ustack_npage = 1;
    p->heap_top = USER_BASE + PGSIZE;

    // LAB-9: 初始化 proczero 的文件系统上下文
    // 注意：此时 fs_init 尚未运行，fs_init 会在 proc_entry_point 中被 PID 1 调用
    // 但我们可以预设路径，等到进程真正开始运行并调用 fs_init 后，文件系统就可用了
    // 这里的初始化放在 proc_entry_point 的 fs_init 之后更安全
    // 所以我们在 proc_entry_point 的 fs_init() 后面补充逻辑 (见下方修改)

    p->tf->user_to_kern_epc = USER_BASE;
    p->tf->sp = TRAPFRAME;
    p->tf->user_to_kern_satp = r_satp();
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;
    extern void trap_user_handler();
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_hartid = r_tp();

    char *s = "proczero";
    for(int i=0; s[i]; i++) p->name[i] = s[i];

    p->state = RUNNABLE;
    spinlock_release(&p->lk);
}

// 修改 proc_entry_point 以支持 proczero 的文件初始化
static void proc_entry_point_updated()
{
    proc_t *p = myproc();
    spinlock_release(&p->lk);

    if (p->pid == 1) {
        fs_init();
        // LAB-9: 只有 PID 1 (init) 需要手动打开标准流
        p->cwd = __path_to_inode(NULL, "/", false);
        p->open_file[0] = file_open("/dev/stdin", O_RDONLY);
        p->open_file[1] = file_open("/dev/stdout", O_WRONLY);
        p->open_file[2] = file_open("/dev/stderr", O_WRONLY);
    }

    trap_user_return();
}
// 注意：请在 proc_alloc 中将 p->ctx.ra 指向 proc_entry_point (此处逻辑已整合)

// 创建子进程 (Fork)
int proc_fork()
{
    proc_t *curr = myproc();
    proc_t *child = proc_alloc(); 
    if (!child) return -1;

    uvm_copy_pgtbl(curr->pgtbl, child->pgtbl, curr->heap_top, curr->ustack_npage, curr->mmap);
    child->heap_top = curr->heap_top;
    child->ustack_npage = curr->ustack_npage;

    mmap_region_t *src = curr->mmap;
    mmap_region_t **dst = &child->mmap;
    while (src) {
        mmap_region_t *new_node = mmap_region_alloc();
        new_node->begin = src->begin;
        new_node->npages = src->npages;
        new_node->next = NULL;
        *dst = new_node;
        dst = &new_node->next;
        src = src->next;
    }

    // LAB-9: 继承当前工作目录
    if(curr->cwd) child->cwd = inode_dup(curr->cwd);

    // LAB-9: 继承打开的文件表 (增加引用计数)
    for(int i = 0; i < N_OPEN_FILE; i++) {
        if(curr->open_file[i]) {
            child->open_file[i] = file_dup(curr->open_file[i]);
        }
    }

    *(child->tf) = *(curr->tf);
    child->tf->a0 = 0; 
    child->tf->user_to_kern_sp = child->kstack + PGSIZE;

    for(int i=0; i<16; i++) child->name[i] = curr->name[i];
    child->parent = curr;
    
    int pid = child->pid;
    child->state = RUNNABLE;
    spinlock_release(&child->lk);
    
    return pid;
}

void proc_sched()
{
    swtch(&myproc()->ctx, &mycpu()->ctx);
}

void proc_yield()
{
    proc_t *p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

void proc_wakeup(void *chan)
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_pool[i];
        if (p != myproc()) {
            spinlock_acquire(&p->lk);
            if (p->state == SLEEPING && p->sleep_space == chan) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
    }
}

void proc_sleep(void *chan, spinlock_t *lk)
{
    proc_t *p = myproc();
    spinlock_acquire(&p->lk);
    spinlock_release(lk);

    p->sleep_space = chan;
    p->state = SLEEPING;

    proc_sched(); 

    p->sleep_space = NULL;
    spinlock_release(&p->lk);
    spinlock_acquire(lk);
}

void proc_scheduler()
{
    cpu_t *c = mycpu();
    c->proc = NULL;

    for (;;) {
        intr_on();
        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &proc_pool[i];
            spinlock_acquire(&p->lk);
            if (p->state == RUNNABLE) {
                p->state = RUNNING;
                c->proc = p;
                #if SCHED_TRACE
                printf("proc %d is running...\n", p->pid);
                #endif
                swtch(&c->ctx, &p->ctx);
                c->proc = NULL;
            }
            spinlock_release(&p->lk);
        }
    }
}

void proc_exit(int code)
{
    proc_t *curr = myproc();
    if (curr == init_process) panic("init process exiting");

    spinlock_acquire(&lifecycle_lock);

    for (int i = 0; i < N_PROC; i++) {
        if (proc_pool[i].parent == curr) {
            proc_pool[i].parent = init_process;
            spinlock_acquire(&proc_pool[i].lk);
            if (proc_pool[i].state == ZOMBIE) {
                 proc_wakeup(init_process); 
            }
            spinlock_release(&proc_pool[i].lk);
        }
    }

    spinlock_acquire(&curr->lk);
    curr->exit_code = code;
    curr->state = ZOMBIE;
    proc_wakeup(curr->parent); 
    spinlock_release(&curr->lk);
    spinlock_release(&lifecycle_lock);

    spinlock_acquire(&curr->lk);
    proc_sched();
    panic("zombie process revived");
}

int proc_wait(uint64 addr)
{
    spinlock_acquire(&lifecycle_lock);

    for (;;) {
        int have_kids = 0;
        proc_t *curr = myproc();

        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &proc_pool[i];
            if (p->parent != curr) continue;
            
            have_kids = 1;
            spinlock_acquire(&p->lk);
            if (p->state == ZOMBIE) {
                int pid = p->pid;
                int code = p->exit_code;
                printf("proc %d is wakeup!\n", curr->pid);
                proc_free(p); 
                spinlock_release(&lifecycle_lock);
                if (addr != 0) {
                    uvm_copyout(curr->pgtbl, addr, (uint64)&code, sizeof(int));
                }
                return pid;
            }
            spinlock_release(&p->lk);
        }

        if (!have_kids) {
            spinlock_release(&lifecycle_lock);
            return -1;
        }
        proc_sleep(curr, &lifecycle_lock);
    }
}