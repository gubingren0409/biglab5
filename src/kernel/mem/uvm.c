#include "mod.h"

/* -------------------------------------------------------------------------
 * Part 1: 用户空间与内核空间的数据传输
 * ------------------------------------------------------------------------- */

/*
 * 从用户空间拷贝数据到内核空间 (copy_from_user)
 * pgtbl: 用户页表
 * dst: 内核目的地址
 * src: 用户源地址 (虚拟地址)
 * len: 拷贝长度
 */
void uvm_copyin(pgtbl_t user_tbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 copied_bytes = 0;
    
    while (copied_bytes < len) {
        uint64 va = src + copied_bytes;
        uint64 page_offset = va % PGSIZE;
        
        // 查找用户地址对应的 PTE
        pte_t *pte = vm_getpte(user_tbl, va, false);
        
        // 权限检查：必须有效(V)、可读(R)、用户可访问(U)
        if (pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_R) || !(*pte & PTE_U)) {
            panic("uvm_copyin: access violation or unmapped addr");
        }
        
        uint64 pa = PTE_TO_PA(*pte);
        uint64 bytes_this_page = PGSIZE - page_offset;
        uint64 remaining = len - copied_bytes;
        
        // 本次拷贝取“页剩余空间”和“总剩余长度”的较小值
        uint64 n = (bytes_this_page < remaining) ? bytes_this_page : remaining;
        
        // 执行拷贝
        memmove((void *)(dst + copied_bytes), (void *)(pa + page_offset), n);
        
        copied_bytes += n;
    }
}

/*
 * 从内核空间拷贝数据到用户空间 (copy_to_user)
 * pgtbl: 用户页表
 * dst: 用户目的地址 (虚拟地址)
 * src: 内核源地址
 * len: 拷贝长度
 */
void uvm_copyout(pgtbl_t user_tbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 copied_bytes = 0;
    
    while (copied_bytes < len) {
        uint64 va = dst + copied_bytes;
        uint64 page_offset = va % PGSIZE;
        
        pte_t *pte = vm_getpte(user_tbl, va, false);
        
        // 权限检查：必须有效(V)、可写(W)、用户可访问(U)
        if (pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_W) || !(*pte & PTE_U)) {
            panic("uvm_copyout: access violation or unmapped addr");
        }
        
        uint64 pa = PTE_TO_PA(*pte);
        uint64 bytes_this_page = PGSIZE - page_offset;
        uint64 remaining = len - copied_bytes;
        
        uint64 n = (bytes_this_page < remaining) ? bytes_this_page : remaining;
        
        memmove((void *)(pa + page_offset), (void *)(src + copied_bytes), n);
        
        copied_bytes += n;
    }
}

/*
 * 从用户空间拷贝字符串到内核空间
 * maxlen: 最大允许长度 (防止溢出)
 * dst: 内核缓冲区
 * src: 用户字符串地址
 */
void uvm_copyin_str(pgtbl_t user_tbl, uint64 dst, uint64 src, uint32 maxlen)
{
    uint64 n = 0;
    char *k_dst = (char *)dst;
    
    while (n < maxlen) {
        uint64 va = src + n;
        pte_t *pte = vm_getpte(user_tbl, va, false);
        
        if (pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_R) || !(*pte & PTE_U)) {
            panic("uvm_copyin_str: invalid user string ptr");
        }
        
        uint64 pa = PTE_TO_PA(*pte);
        uint64 offset = va % PGSIZE;
        char *p_str = (char *)(pa + offset);
        
        // 逐字节拷贝直到页边界或遇到 '\0'
        while (n < maxlen && offset < PGSIZE) {
            char c = *p_str;
            k_dst[n] = c;
            
            if (c == '\0') return; // 拷贝完成
            
            n++;
            offset++;
            p_str++;
        }
    }
    
    // 强制结尾，防止未截断
    k_dst[maxlen - 1] = '\0';
}

/* -------------------------------------------------------------------------
 * Part 2: mmap 区域管理 (链表 + 映射)
 * ------------------------------------------------------------------------- */

// 调试工具：打印进程的 mmap 链表
void uvm_show_mmaplist(mmap_region_t *head)
{
    printf("\n=== Process MMAP List ===\n");
    mmap_region_t *node = head;
    while (node) {
        printf("[%p - %p] pages=%d\n", 
               node->begin, 
               node->begin + node->npages * PGSIZE, 
               node->npages);
        node = node->next;
    }
    if (!head) printf("(empty)\n");
}

// 辅助函数：寻找一块足够大的虚拟地址空间
static uint64 find_free_region(mmap_region_t *head, uint32 npages, mmap_region_t **out_prev)
{
    uint64 req_len = npages * PGSIZE;
    uint64 candidate_start = MMAP_BEGIN;
    
    mmap_region_t *curr = head;
    mmap_region_t *prev = NULL;
    
    // 遍历链表寻找空隙
    while (curr != NULL) {
        // 检查 [candidate_start, curr->begin) 是否够大
        if (curr->begin >= candidate_start + req_len) {
            if (out_prev) *out_prev = prev;
            return candidate_start;
        }
        
        // 移动候选起始点到当前区域之后
        candidate_start = curr->begin + curr->npages * PGSIZE;
        prev = curr;
        curr = curr->next;
    }
    
    // 检查最后一个区域之后是否有空间
    if (candidate_start + req_len <= MMAP_END) {
        if (out_prev) *out_prev = prev;
        return candidate_start;
    }
    
    return 0; // 无空间
}

/*
 * 建立新的内存映射
 */
void uvm_mmap(uint64 start, uint32 npages, int perm)
{
    proc_t *p = myproc();
    mmap_region_t *prev_node = NULL;
    uint64 map_addr;
    
    // 1. 确定映射地址
    if (start == 0) {
        // 自动分配模式
        map_addr = find_free_region(p->mmap, npages, &prev_node);
        if (map_addr == 0) panic("uvm_mmap: out of virtual memory");
    } else {
        // 指定地址模式
        map_addr = start;
        uint64 map_end = start + npages * PGSIZE;
        
        // 检查越界
        if (map_addr < MMAP_BEGIN || map_end > MMAP_END) 
            panic("uvm_mmap: invalid address range");
            
        // 寻找插入位置 (保持链表有序)
        mmap_region_t *curr = p->mmap;
        while (curr && curr->begin < map_addr) {
            // 检查是否与当前节点重叠
            if (curr->begin + curr->npages * PGSIZE > map_addr)
                panic("uvm_mmap: overlap detected");
            prev_node = curr;
            curr = curr->next;
        }
        // 检查是否与后一个节点重叠
        if (curr && map_end > curr->begin)
            panic("uvm_mmap: overlap detected");
    }
    
    // 2. 创建并插入节点
    mmap_region_t *new_node = mmap_region_alloc();
    new_node->begin = map_addr;
    new_node->npages = npages;
    
    // 插入链表
    if (prev_node == NULL) {
        // 插在头部
        new_node->next = p->mmap;
        p->mmap = new_node;
    } else {
        // 插在中间或尾部
        new_node->next = prev_node->next;
        prev_node->next = new_node;
    }
    
    // 3. 尝试合并 (Merge)
    // 检查是否可以与 后一个节点 合并
    if (new_node->next) {
        mmap_region_t *next_node = new_node->next;
        uint64 my_end = new_node->begin + new_node->npages * PGSIZE;
        if (my_end == next_node->begin) {
            new_node->npages += next_node->npages;
            new_node->next = next_node->next;
            mmap_region_free(next_node);
        }
    }
    // 检查是否可以与 前一个节点 合并
    if (prev_node) {
        uint64 prev_end = prev_node->begin + prev_node->npages * PGSIZE;
        if (prev_end == new_node->begin) {
            prev_node->npages += new_node->npages;
            prev_node->next = new_node->next;
            mmap_region_free(new_node);
        }
    }
    
    // 4. 分配物理内存并建立页表映射
    uint64 va = map_addr;
    for (int i = 0; i < npages; i++) {
        void *pa = pmem_alloc(false); // 分配用户物理页
        if (!pa) panic("uvm_mmap: pmem alloc failed");
        
        memset(pa, 0, PGSIZE); // 清零
        vm_mappages(p->pgtbl, va, (uint64)pa, PGSIZE, perm);
        va += PGSIZE;
    }
}

/*
 * 解除内存映射
 */
void uvm_munmap(uint64 start, uint32 npages)
{
    proc_t *p = myproc();
    uint64 unmap_end = start + npages * PGSIZE;
    
    if (start < MMAP_BEGIN || unmap_end > MMAP_END)
        panic("uvm_munmap: address out of range");
        
    mmap_region_t *walker = p->mmap;
    mmap_region_t *prev = NULL;
    
    while (walker) {
        uint64 region_end = walker->begin + walker->npages * PGSIZE;
        
        // 检查区间是否有交集
        if (unmap_end <= walker->begin) {
            break; 
        }
        if (start >= region_end) {
            prev = walker;
            walker = walker->next;
            continue;
        }
        
        // 有交集，计算实际需要解映射的范围
        uint64 overlap_start = (start > walker->begin) ? start : walker->begin;
        uint64 overlap_end = (unmap_end < region_end) ? unmap_end : region_end;
        uint64 overlap_len = overlap_end - overlap_start;
        
        // 1. 执行页表解映射和物理页释放
        vm_unmappages(p->pgtbl, overlap_start, overlap_len, true);
        
        // 2. 更新链表节点结构
        if (start <= walker->begin && unmap_end >= region_end) {
            // Case A: 完全覆盖 (Remove Node)
            mmap_region_t *victim = walker;
            if (prev) prev->next = walker->next;
            else p->mmap = walker->next;
            
            walker = walker->next; 
            mmap_region_free(victim);
            continue; 
        }
        else if (start <= walker->begin && unmap_end < region_end) {
            // Case B: 头部截断 (Trim Head)
            uint32 cut_pages = (unmap_end - walker->begin) / PGSIZE;
            walker->begin = unmap_end;
            walker->npages -= cut_pages;
            break;
        }
        else if (start > walker->begin && unmap_end >= region_end) {
            // Case C: 尾部截断 (Trim Tail)
            uint32 cut_pages = (region_end - start) / PGSIZE;
            walker->npages -= cut_pages;
            prev = walker;
            walker = walker->next;
            continue; 
        }
        else {
            // Case D: 中间打洞 (Split)
            mmap_region_t *new_node = mmap_region_alloc();
            new_node->begin = unmap_end;
            new_node->npages = (region_end - unmap_end) / PGSIZE;
            new_node->next = walker->next;
            
            walker->npages = (start - walker->begin) / PGSIZE;
            walker->next = new_node;
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Part 3: 堆栈管理 (Heap & Stack)
 * ------------------------------------------------------------------------- */

/*
 * [修复] 堆增长
 */
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len) 
{
    uint64 new_heap_top = cur_heap_top + len;
    
    // 从当前 top 向上对齐到页边界
    uint64 a = (cur_heap_top + PGSIZE - 1) & ~(PGSIZE - 1);
    
    for (; a < new_heap_top; a += PGSIZE) {
        // 分配用户页 (参数 false)
        void *mem = pmem_alloc(false);
        if (mem == NULL) {
            // 分配失败，回滚操作（可选，这里简单返回0）
            // 在实际系统中可能需要 unmap 之前分配的页
            return 0; 
        }
        memset(mem, 0, PGSIZE);
        
        // 建立映射，堆通常是可读写的，且用户可访问
        vm_mappages(pgtbl, a, (uint64)mem, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    
    return new_heap_top;
}

// 堆收缩
uint64 uvm_heap_ungrow(pgtbl_t tbl, uint64 current_top, uint32 bytes)
{
    if (bytes == 0) return current_top;
    
    uint64 new_top = (current_top > bytes) ? (current_top - bytes) : 0;
    
    // 计算需要释放的页范围
    uint64 page_aligned_curr = (current_top + PGSIZE - 1) & ~(PGSIZE - 1);
    uint64 page_aligned_new = (new_top + PGSIZE - 1) & ~(PGSIZE - 1);
    
    // 保护代码段和初始数据段，不能释放到 USER_BASE 以下
    if (page_aligned_new < USER_BASE + PGSIZE)
        page_aligned_new = USER_BASE + PGSIZE;
        
    if (page_aligned_curr > page_aligned_new) {
        vm_unmappages(tbl, page_aligned_new, page_aligned_curr - page_aligned_new, true);
    }
    
    return new_top;
}

// 用户栈自动增长 (Handle Page Fault)
uint64 uvm_ustack_grow(pgtbl_t tbl, uint64 current_pages, uint64 fault_addr)
{
    if (fault_addr >= TRAPFRAME) return (uint64)-1;
    if (fault_addr < MMAP_END) return (uint64)-1;
    
    uint64 current_bottom = TRAPFRAME - current_pages * PGSIZE;
    
    if (fault_addr >= current_bottom) return current_pages;
    
    uint64 target_bottom = fault_addr & ~(PGSIZE - 1);
    uint32 pages_needed = (current_bottom - target_bottom) / PGSIZE;
    
    for (int i = 0; i < pages_needed; i++) {
        uint64 va = current_bottom - (i + 1) * PGSIZE;
        void *mem = pmem_alloc(false);
        if (!mem) {
            if (i > 0) {
                vm_unmappages(tbl, va + PGSIZE, i * PGSIZE, true);
            }
            return (uint64)-1;
        }
        vm_mappages(tbl, va, (uint64)mem, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    
    return current_pages + pages_needed;
}

/* -------------------------------------------------------------------------
 * Part 4: 页表生命周期 (Copy & Destroy)
 * ------------------------------------------------------------------------- */

// 递归销毁页表及其映射的物理内存
static void free_pagetable_recursive(pgtbl_t tbl, int level)
{
    for (int i = 0; i < 512; i++) {
        pte_t pte = tbl[i];
        if (pte & PTE_V) {
            uint64 child_pa = PTE_TO_PA(pte);
            
            if (level > 0) {
                // 中间层：递归释放下一级页表
                free_pagetable_recursive((pgtbl_t)child_pa, level - 1);
            } else {
                // 叶子层：如果是用户页 (PTE_U)，则释放物理内存
                if (pte & PTE_U) {
                    pmem_free(child_pa, false);
                }
            }
        }
    }
    // 释放当前页表页本身
    pmem_free((uint64)tbl, true);
}

// 销毁进程页表
void uvm_destroy_pgtbl(pgtbl_t tbl)
{
    free_pagetable_recursive(tbl, 2); // SV39 顶层为 level 2
}

// 辅助：拷贝一段虚拟地址范围的内存
static int copy_virt_range(pgtbl_t src_tbl, pgtbl_t dst_tbl, uint64 start, uint64 end)
{
    for (uint64 va = start; va < end; va += PGSIZE) {
        pte_t *src_pte = vm_getpte(src_tbl, va, false);
        if (!src_pte || !(*src_pte & PTE_V)) 
            panic("uvm_copy: source pte missing");
            
        uint64 src_pa = PTE_TO_PA(*src_pte);
        int flags = PTE_FLAGS(*src_pte);
        
        // 分配新物理页
        void *dst_pa = pmem_alloc(false);
        if (!dst_pa) return -1;
        
        // 深拷贝内存内容
        memmove(dst_pa, (void *)src_pa, PGSIZE);
        
        // 建立新映射
        vm_mappages(dst_tbl, va, (uint64)dst_pa, PGSIZE, flags);
    }
    return 0;
}

// 复制父进程的地址空间到子进程 (Fork)
void uvm_copy_pgtbl(pgtbl_t old_tbl, pgtbl_t new_tbl, uint64 heap_top, uint64 ustack_pages, mmap_region_t *mmap_head)
{
    // 1. 复制代码段
    copy_virt_range(old_tbl, new_tbl, USER_BASE, USER_BASE + PGSIZE);
    
    // 2. 复制堆
    if (heap_top > USER_BASE + PGSIZE) {
        uint64 heap_end = (heap_top + PGSIZE - 1) & ~(PGSIZE - 1);
        copy_virt_range(old_tbl, new_tbl, USER_BASE + PGSIZE, heap_end);
    }
    
    // 3. 复制栈
    if (ustack_pages > 0) {
        uint64 stack_base = TRAPFRAME - ustack_pages * PGSIZE;
        copy_virt_range(old_tbl, new_tbl, stack_base, TRAPFRAME);
    }
    
    // 4. 复制 mmap 区域
    mmap_region_t *walker = mmap_head;
    while (walker) {
        uint64 end = walker->begin + walker->npages * PGSIZE;
        copy_virt_range(old_tbl, new_tbl, walker->begin, end);
        walker = walker->next;
    }
}