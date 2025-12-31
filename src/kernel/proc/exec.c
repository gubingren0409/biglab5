#include "mod.h"

int proc_exec(char *path, char **argv) {
    struct elfhdr elf;
    struct proghdr ph;
    pagetable_t pagetable = 0, old_pagetable;
    inode_t *ip;
    uint64 sz = 0, sp, stackbase;
    proc_t *p = curr_proc();

    if ((ip = __path_to_inode(NULL, path, false)) == 0) return -1;
    acquire_sleeplock(&ip->lk);

    // Step 2: Check ELF header
    if (inode_read(ip, (uint64)&elf, 0, sizeof(elf), false) != sizeof(elf) || elf.magic != ELF_MAGIC)
        goto bad;

    if ((pagetable = kvm_create_pagetable()) == 0) goto bad;

    // Step 3: Load segments to heap
    for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (inode_read(ip, (uint64)&ph, off, sizeof(ph), false) != sizeof(ph)) goto bad;
        if (ph.type != ELF_PROG_LOAD) continue;
        if (ph.memsz < ph.filesz) goto bad;
        
        uint32 flags = PTE_R | PTE_U;
        if (ph.flags & ELF_PROG_FLAG_WRITE) flags |= PTE_W;
        if (ph.flags & ELF_PROG_FLAG_EXEC) flags |= PTE_X;

        uint64 new_sz = uvm_heap_grow(pagetable, sz, ph.vaddr + ph.memsz, flags);
        if (new_sz == sz && ph.vaddr + ph.memsz > sz) goto bad;
        sz = new_sz;
        
        if (load_segment(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
    }
    release_sleeplock(&ip->lk);
    inode_put(ip);
    ip = 0;

    // Step 5: Stack and Args
    sz = PG_ROUND_UP(sz);
    uint64 new_sz = uvm_heap_grow(pagetable, sz, sz + USTACK_NPAGE * PG_SIZE, PTE_R|PTE_W|PTE_U);
    if (new_sz == sz) goto bad;
    
    stackbase = new_sz - USTACK_NPAGE * PG_SIZE;
    sp = prepare_stack(pagetable, new_sz, argv);
    if (sp == 0) goto bad;

    // Step 6-8: Commit changes
    old_pagetable = p->pagetable;
    p->pagetable = pagetable;
    p->heap_top = sz;
    p->ustack_npage = USTACK_NPAGE;
    p->trapframe->epc = elf.entry;
    p->trapframe->sp = sp;
    
    // Copy argc to a0, argv to a1 (simplified)
    int argc = 0; while(argv[argc]) argc++;
    p->trapframe->a0 = argc;
    // p->trapframe->a1 = ... (pointer to argv on stack)

    kvm_free_pagetable(old_pagetable, true);
    return 0;

bad:
    if (pagetable) kvm_free_pagetable(pagetable, true);
    if (ip) { release_sleeplock(&ip->lk); inode_put(ip); }
    return -1;
}