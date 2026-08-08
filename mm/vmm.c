/*
 * vmm.c - 虚拟内存管理器
 * x86 两级分页：页目录/页表管理、虚拟地址映射、页目录切换。
 * 早期用身份映射（虚拟地址 == 物理地址），覆盖全物理内存。
 */

#include "../include/mm/vmm.h"
#include "../include/mm/pmm.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* current_directory 不直接读 CR3：CR3 存的是物理地址，身份映射下
 * 两者相等，但留个指针后面切换目录要用 */
static uint32_t* current_directory = NULL;

/* aligned(4096)：CR3 要求页目录 4KB 对齐；1024 项正好一页 */
static uint32_t kernel_page_directory[1024] __attribute__((aligned(4096)));

/* 不再定义 kernel_first_page_table：旧代码里它是从未使用的静态数组，
 * vmm_destroy_directory 拿它当"内核静态页表"比较，判据永不成立，
 * 共享页表会被错误释放。页表现在全由 PMM 动态分配。 */


static inline void zero_memory(void* ptr, size_t size) {
    unsigned char* p = (unsigned char*)ptr;
    while (size--) *p++ = 0;
}

static inline void set_entry(uint32_t* entry, uint32_t phys_addr, uint32_t flags) {
    *entry = (phys_addr & 0xFFFFF000) | (flags & 0xFFF) | PAGE_PRESENT;
}

static inline void clear_entry(uint32_t* entry) {
    *entry = 0;
}

static inline bool is_present(uint32_t entry) {
    return (entry & PAGE_PRESENT) != 0;
}

static inline uint32_t pd_index(uint32_t virt) { return virt >> 22; }

static inline uint32_t pt_index(uint32_t virt) { return (virt >> 12) & 0x3FF; }

static uint32_t* allocate_pt(void) {
    uint32_t* pt = (uint32_t*)pmm_alloc_page();
    if (pt) {
        zero_memory(pt, 1024 * sizeof(uint32_t));
    }
    return pt;
}

static void free_pt(uint32_t* pt) {
    if (pt) pmm_free_page((void*)pt);
}


/* 初始化分页：身份映射全物理内存，写 CR3 并打开 CR0.PG */
void vmm_init(void) {
    zero_memory(kernel_page_directory, sizeof(kernel_page_directory));

    size_t total_pages = pmm_get_memory_size() / 4096;
    if (total_pages == 0) {
        vga_write("[VMM] ERROR: No memory detected!\n");
        return;
    }

    /* 每张页表管 4MB，页目录一共 1024 个槽 */
    uint32_t num_page_tables = (total_pages + 1023) / 1024;
    if (num_page_tables > 1024) num_page_tables = 1024;

    for (uint32_t i = 0; i < num_page_tables; i++) {

        uint32_t* pt = (uint32_t*)pmm_alloc_page();
        if (!pt) {
            vga_write("[VMM] ERROR: Failed to allocate page table!\n");
            return;
        }

        zero_memory(pt, 1024 * sizeof(uint32_t));

        /* 第 i 张页表对应的物理基地址 = i * 4MB */
        uint32_t base_phys = i * 1024 * 4096;

        for (uint32_t j = 0; j < 1024; j++) {
            uint32_t phys = base_phys + j * 4096;

            /* 超出物理内存的项留空，访问会触发 #PF */
            if (phys >= pmm_get_memory_size()) break;

            /* 0x02：set_entry 会自动加 PRESENT，这里只传 WRITE */
            set_entry(&pt[j], phys, 0x02);
        }

        set_entry(&kernel_page_directory[i], (uint32_t)pt, 0x02);
    }

    /* 身份映射下写 CR3 不会打断当前指令流 */
    current_directory = kernel_page_directory;
    vmm_switch_directory(current_directory);

    /* ⚠️ 修复：以前只写 CR3 没置 CR0.PG，分页从未真正生效——页表只是摆设。
     * 低地址时碰巧正常，用户空间搬到 256MB（物理不存在）后必崩。
     * 现在打开 PG 位，identity 映射立即生效。 */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;   /* CR0.PG = 1 */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    vga_write("[VMM] Paging enabled with identity mapping over all physical memory.\n");
}


/* 创建用户进程的独立页目录，失败返回 NULL */
uint32_t* vmm_create_directory(void) {
    uint32_t* dir = (uint32_t*)pmm_alloc_page();
    if (!dir) return NULL;

    zero_memory(dir, 1024 * sizeof(uint32_t));

    /* ⚠️ 修复：复制内核**全部** PDE，用户目录共享内核页表
     * （PTE 无 USER 位，用户态访问照样 #PF）。旧实现只复制 PDE[0]，
     * 用户程序映射 4MB+ 区域会跟内核身份映射冲突，内核栈页在 4MB 以上时
     * 用户目录下不可访问。 */
    for (uint32_t i = 0; i < 1024; i++) {
        dir[i] = kernel_page_directory[i];
    }

    return dir;
}


/* 销毁页目录：释放名下所有非内核页表，再释放页目录自身。
 * 调用方须保证没有进程还在用这些页表。 */
void vmm_destroy_directory(uint32_t* dir) {
    if (!dir) return;

    /* ⚠️ 修复：所有与内核页目录相同的 PDE 都要跳过（共享页表绝不释放）。
     * 旧实现只跳过 PDE[0]，会误释放用户目录复制进来的其他内核页表。 */
    uint32_t kernel_pdes = (uint32_t)(pmm_get_memory_size() / (1024 * 4096)) + 1;
    if (kernel_pdes > 1024) kernel_pdes = 1024;

    for (uint32_t i = 0; i < 1024; i++) {
        if (is_present(dir[i])) {
            /* PDE 高 20 位是页表物理地址 */
            uint32_t pt_phys = dir[i] & 0xFFFFF000;

            bool is_kernel = false;
            for (uint32_t k = 0; k < kernel_pdes; k++) {
                if (is_present(kernel_page_directory[k]) &&
                    (kernel_page_directory[k] & 0xFFFFF000) == pt_phys) {
                    is_kernel = true;
                    break;
                }
            }
            if (is_kernel) continue;

            uint32_t* pt = (uint32_t*)pt_phys;
            free_pt(pt);
        }
    }

    /* 释放页目录本身占用的物理页 */
    pmm_free_page((void*)dir);
}


/* 建立 virt→phys 映射。页表不存在会自动分配；映射已存在则失败 */
bool vmm_map_page(uint32_t* dir, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    if (!dir) return false;

    virt_addr &= 0xFFFFF000;
    phys_addr &= 0xFFFFF000;

    uint32_t pd_idx = pd_index(virt_addr);
    uint32_t pt_idx = pt_index(virt_addr);

    uint32_t* pt = NULL;

    if (!is_present(dir[pd_idx])) {
        /* 页表不存在，先分配一张 */
        pt = allocate_pt();
        if (!pt) return false;

        /* PTE 要 USER 的话 PDE 也得带 USER，否则遍历到 PDE 就被拒 */
        uint32_t pde_flags = PAGE_PRESENT | PAGE_WRITE;
        if (flags & PAGE_USER) pde_flags |= PAGE_USER;

        set_entry(&dir[pd_idx], (uint32_t)pt, pde_flags);

    } else {
        if (flags & PAGE_USER) {
            /* ⚠️ 共享内核页表绝不能加 USER 位，否则物理内存对用户态开放 */
            uint32_t pt_phys = dir[pd_idx] & 0xFFFFF000;
            uint32_t kernel_pdes = (uint32_t)(pmm_get_memory_size() / (1024 * 4096)) + 1;
            if (kernel_pdes > 1024) kernel_pdes = 1024;
            bool is_kernel = false;
            for (uint32_t k = 0; k < kernel_pdes; k++) {
                if (is_present(kernel_page_directory[k]) &&
                    (kernel_page_directory[k] & 0xFFFFF000) == pt_phys) {
                    is_kernel = true;
                    break;
                }
            }
            if (is_kernel) return false;

            dir[pd_idx] |= PAGE_USER;
        }

        uint32_t pt_phys = dir[pd_idx] & 0xFFFFF000;
        pt = (uint32_t*)pt_phys;
    }

    /* PTE 已存在就拒绝，不覆盖已有映射；要重映射先 unmap */
    if (is_present(pt[pt_idx])) return false;

    set_entry(&pt[pt_idx], phys_addr, flags | PAGE_PRESENT);

    /* TLB 里可能还缓存着旧映射，invlpg 刷掉这一页 */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");

    return true;
}


/* 解除映射（只清 PTE，物理页不还，调用方自己处理） */
bool vmm_unmap_page(uint32_t* dir, uint32_t virt_addr) {
    if (!dir) return false;

    virt_addr &= 0xFFFFF000;
    uint32_t pd_idx = pd_index(virt_addr);
    uint32_t pt_idx = pt_index(virt_addr);

    if (!is_present(dir[pd_idx])) return false;

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & 0xFFFFF000);

    if (!is_present(pt[pt_idx])) return false;

    clear_entry(&pt[pt_idx]);

    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");

    return true;
}


/* 检查虚拟地址是否已映射且带 PAGE_USER（syscall 校验用户指针用） */
bool vmm_is_user_accessible(uint32_t virt_addr) {
    if (!current_directory) return false;

    uint32_t pd_idx = pd_index(virt_addr);
    uint32_t pt_idx = pt_index(virt_addr);

    if (!is_present(current_directory[pd_idx])) return false;

    uint32_t* pt = (uint32_t*)(current_directory[pd_idx] & 0xFFFFF000);

    if (!is_present(pt[pt_idx])) return false;
    if (!(pt[pt_idx] & PAGE_USER)) return false;

    return true;
}

/* 手动走一遍页表查物理地址（含页内偏移），未映射返回 0 */
uint32_t vmm_get_phys_addr(uint32_t* dir, uint32_t virt_addr) {
    if (!dir) return 0;

    uint32_t pd_idx = pd_index(virt_addr);
    uint32_t pt_idx = pt_index(virt_addr);

    if (!is_present(dir[pd_idx])) return 0;

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & 0xFFFFF000);

    if (!is_present(pt[pt_idx])) return 0;

    uint32_t phys = pt[pt_idx] & 0xFFFFF000;
    return phys | (virt_addr & 0xFFF);
}


/* 切换页目录（进程切换的关键步骤；写 CR3 自动刷整个 TLB） */
void vmm_switch_directory(uint32_t* dir) {
    if (!dir) return;
    current_directory = dir;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)dir) : "memory");
}


uint32_t* vmm_get_current_directory(void) {
    return current_directory;
}
