#include "mod.h"

// -------------------------------------------------------------------
// ELF 格式定义 (通常在 elf.h)
// -------------------------------------------------------------------
#define ELF_MAGIC 0x464C457F

// ELF 文件头
struct elfhdr {
    uint32 magic;
    uint8 elf[12];
    uint16 type;
    uint16 machine;
    uint32 version;
    uint64 entry;
    uint64 phoff;
    uint64 shoff;
    uint32 version2;
    uint32 flags;
    uint16 ehsize;
    uint16 phentsize;
    uint16 phnum;
    uint16 shentsize;
    uint16 shnum;
    uint16 shstrndx;
};

// 程序段头
struct proghdr {
    uint32 type;
    uint32 flags;
    uint64 off;
    uint64 vaddr;
    uint64 paddr;
    uint64 filesz;
    uint64 memsz;
    uint64 align;
};

#define ELF_PROG_LOAD           1
#define ELF_PROG_FLAG_EXEC      1
#define ELF_PROG_FLAG_WRITE     2
#define ELF_PROG_FLAG_READ      4

// -------------------------------------------------------------------
// 辅助宏与常量
// -------------------------------------------------------------------
#ifndef USTACK_NPAGE
#define USTACK_NPAGE 1
#endif

// -------------------------------------------------------------------
// 内部辅助函数
// -------------------------------------------------------------------

/* 在页表中分配指定虚拟地址范围的内存 */
static int uvm_alloc(pgtbl_t pgtbl, uint64 start, uint64 end, int perm) {
    // 对齐到页边界
    uint64 a = ALIGN_DOWN(start, PGSIZE);
    uint64 last = ALIGN_UP(end, PGSIZE);
    
    for (; a < last; a += PGSIZE) {
        void *mem = pmem_alloc(false);
        if (mem == NULL) return -1;
        memset(mem, 0, PGSIZE);
        
        // 映射物理页 (mem 被视为内核直接映射地址，即 PA)
        // vm_mappages 返回 void，出错会直接 panic，所以这里不检查返回值
        vm_mappages(pgtbl, a, (uint64)mem, PGSIZE, perm);
    }
    return 0;
}

/* 将文件段加载到页表对应的物理内存中 */
static int load_segment(pgtbl_t pgtbl, uint64 va, inode_t *ip, uint32 offset, uint32 sz) {
    uint32 i, n;
    uint64 pa;
    pte_t *pte;

    for (i = 0; i < sz; i += n) {
        pte = vm_getpte(pgtbl, va + i, false);
        if (!pte || !(*pte & PTE_V)) return -1;
        
        pa = PTE_TO_PA(*pte);
        n = PGSIZE - ((va + i) % PGSIZE);
        if (n > sz - i) n = sz - i;

        // 读取数据到物理地址 (注意: inode_read_data 参数顺序为 ip, offset, len, dst, is_user)
        // 这里 pa 是内核可直接访问的地址
        if (inode_read_data(ip, offset + i, n, (void *)pa, false) != n)
            return -1;
    }
    return 0;
}

/* 准备用户栈 (压入 argv 和字符串) */
static uint64 prepare_stack(pgtbl_t pgtbl, uint64 top, char **argv) {
    uint64 sp = top;
    uint64 stack[32]; // 最多支持 32 个参数
    int argc = 0;
    char *s;

    // 1. 压入字符串参数
    for (; argv[argc]; argc++) {
        if (argc >= 32) return 0;
        s = argv[argc];
        sp -= strlen(s) + 1;
        sp -= sp % 16; // riscv sp 必须 16 字节对齐
        if (sp < top - USTACK_NPAGE * PGSIZE) return 0; // 栈溢出
        
        // 使用 copyout 将字符串拷贝到新栈
        uvm_copyout(pgtbl, sp, (uint64)s, strlen(s) + 1);
        stack[argc] = sp; // 记录字符串地址
    }
    stack[argc] = 0; // argv 结束符

    // 2. 压入 argv 数组
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < top - USTACK_NPAGE * PGSIZE) return 0;
    
    uvm_copyout(pgtbl, sp, (uint64)stack, (argc + 1) * sizeof(uint64));
    
    // 返回最终的栈顶 (也是 argv 数组的起始地址，即 a1 的值)
    return sp;
}

// -------------------------------------------------------------------
// proc_exec 实现
// -------------------------------------------------------------------

int proc_exec(char *path, char **argv) {
    struct elfhdr elf;
    struct proghdr ph;
    pgtbl_t pgtbl = 0, old_pgtbl;
    inode_t *ip;
    uint64 sz = 0, sp;
    proc_t *p = myproc();

    // Step 1: 获取 Inode
    if ((ip = __path_to_inode(NULL, path, false)) == 0) return -1;
    sleeplock_acquire(&ip->slk);

    // Step 2: 检查 ELF 头
    // 参数: ip, offset, len, dst, is_user
    if (inode_read_data(ip, 0, sizeof(elf), (void *)&elf, false) != sizeof(elf) || elf.magic != ELF_MAGIC)
        goto bad;

    // 创建新页表 (映射 Trapframe 和 Trampoline)
    if ((pgtbl = proc_pgtbl_init((uint64)p->tf)) == 0) goto bad;

    // Step 3: 加载段到内存
    for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (inode_read_data(ip, off, sizeof(ph), (void *)&ph, false) != sizeof(ph)) goto bad;
        if (ph.type != ELF_PROG_LOAD) continue;
        if (ph.memsz < ph.filesz) goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr) goto bad; // 溢出检查
        
        int perm = PTE_R | PTE_U;
        if (ph.flags & ELF_PROG_FLAG_WRITE) perm |= PTE_W;
        if (ph.flags & ELF_PROG_FLAG_EXEC) perm |= PTE_X;

        // 分配并映射内存
        if (uvm_alloc(pgtbl, ph.vaddr, ph.vaddr + ph.memsz, perm) < 0) goto bad;
        
        // 记录最大的堆地址
        uint64 end = ALIGN_UP(ph.vaddr + ph.memsz, PGSIZE);
        if (end > sz) sz = end;
        
        // 加载数据
        if (load_segment(pgtbl, ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
    }
    sleeplock_release(&ip->slk);
    inode_put(ip);
    ip = 0;

    // Step 5: 分配用户栈
    sz = ALIGN_UP(sz, PGSIZE);
    // 在堆顶上方分配栈
    uint64 stack_base = sz;
    uint64 stack_top = stack_base + USTACK_NPAGE * PGSIZE;
    
    if (uvm_alloc(pgtbl, stack_base, stack_top, PTE_R | PTE_W | PTE_U) < 0) goto bad;
    
    // Step 5.1: 准备栈内容
    sp = prepare_stack(pgtbl, stack_top, argv);
    if (sp == 0) goto bad;

    // Step 6-8: 提交更改
    old_pgtbl = p->pgtbl;
    p->pgtbl = pgtbl;
    p->heap_top = stack_top; 
    p->ustack_npage = USTACK_NPAGE;
    
    // [修复] trapframe 成员名修正: epc -> user_to_kern_epc
    p->tf->user_to_kern_epc = elf.entry; 
    p->tf->sp = sp;
    
    // 设置参数 a0 = argc, a1 = argv
    int argc = 0; while(argv[argc]) argc++;
    p->tf->a0 = argc;
    p->tf->a1 = sp; // argv 数组的地址就是栈顶指针
    
    // 拷贝进程名
    char *name, *last;
    last = path;
    for(name=path; *name; name++)
        if(*name == '/') last = name+1;
    memmove(p->name, last, sizeof(p->name));

    // 释放旧页表
    uvm_destroy_pgtbl(old_pgtbl);
    return 0;

bad:
    if (pgtbl) uvm_destroy_pgtbl(pgtbl);
    if (ip) { sleeplock_release(&ip->slk); inode_put(ip); }
    return -1;
}