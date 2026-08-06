/*
 * ============================================================
 * vmm.c — 虚拟内存管理器 (Virtual Memory Manager)
 *
 * 实现 x86 分页机制（Paging），负责页目录/页表的管理、
 * 虚拟地址到物理地址的映射、以及页目录切换。
 * ============================================================
 *
 * ┌────────────────────────────────────────────────────────────┐
 * │  x86 两级分页结构（32位模式）                              │
 * │                                                            │
 * │  虚拟地址分解（CR3 中保存页目录物理地址）：                 │
 * │                                                            │
 * │   31            22  21          12  11                 0   │
 * │   ┌────────────────┬───────────────┬──────────────────┐   │
 * │   │  页目录索引     │  页表索引      │  页内偏移         │   │
 * │   │  (10位)        │  (10位)       │  (12位)           │   │
 * │   └────────────────┴───────────────┴──────────────────┘   │
 * │      索引 0~1023     索引 0~1023     偏移 0~4095          │
 * │                                                            │
 * │  地址转换过程：                                             │
 * │                                                            │
 * │   CR3 ─────→ ┌──────────────┐                              │
 * │              │  页目录 (PD)   │  ← 1024 个 PDE（4字节×1024） │
 * │              │  PDE[0]       │                              │
 * │              │  PDE[1]       │  ── 页目录项中包含页表物理地址 │
 * │              │  ...          │                              │
 * │              │  PDE[1023]    │                              │
 * │              └──────┬───────┘                              │
 * │                     ↓                                      │
 * │              ┌──────────────┐                              │
 * │              │  页表 (PT)    │  ← 1024 个 PTE                │
 * │              │  PTE[0]      │                              │
 * │              │  PTE[1]      │  ── 页表项中包含物理页基址     │
 * │              │  ...         │                              │
 * │              │  PTE[1023]   │                              │
 * │              └──────┬───────┘                              │
 * │                     ↓                                      │
 * │               物理页框 (4KB)  + 页内偏移 → 最终物理地址     │
 * │                                                            │
 * │  一个页目录可管 1024×1024×4KB = 4GB 虚拟地址空间。          │
 * │  每个页表管理 1024×4KB = 4MB 连续虚拟地址。                │
 * └────────────────────────────────────────────────────────────┘
 *
 * ┌────────────────────────────────────────────────────────────┐
 * │  页表项/页目录项标志位（低 12 位）                          │
 * │                                                            │
 * │  位  名称     含义                                         │
 * │  0   P         Present 存在位（1=有效，0=无效，访问触发PF） │
 * │  1   R/W       读/写（0=只读，1=读写）                     │
 * │  2   U/S       用户/超级用户（0=特权级，1=用户可访问）      │
 * │  3   PWT       页级写透                                    │
 * │  4   PCD       页级缓存禁用                                 │
 * │  5   A         访问位（CPU自动置1）                        │
 * │  6   D         脏位（仅PTE，CPU在写入时置1）               │
 * │  7   PS        页大小（仅PDE，0=4KB页，1=4MB大页）         │
 * │  8   G         全局页                                      │
 * │  9-11 可用     操作系统使用                                 │
 * │  12-31 地址    物理页基址（4KB对齐，低12位为0）            │
 * └────────────────────────────────────────────────────────────┘
 *
 * ┌────────────────────────────────────────────────────────────┐
 * │  身份映射策略 (Identity Mapping)                           │
 * │                                                            │
 * │  本实现中使用的映射策略：虚拟地址 = 物理地址                │
 * │  即：0x00000000 映射到物理 0x00000000                      │
 * │      0x00100000 映射到物理 0x00100000                      │
 * │      0x10000000 映射到物理 0x10000000                      │
 * │      依此类推...                                           │
 * │                                                            │
 * │  为什么在早期阶段使用身份映射？                             │
 * │    1. 简单直接，无需建立复杂的虚拟→物理转换表              │
 * │    2. 开启分页的瞬间，指令指针（EIP）仍指向物理地址        │
 * │    3. 内核链接地址与加载地址一致时使用                     │
 * │                                                            │
 * │  缺点：无法提供地址空间隔离                               │
 * │  改进：后续进程切换时可创建独立页目录                      │
 * └────────────────────────────────────────────────────────────┘
 * ============================================================
 */

#include "../include/mm/vmm.h"
#include "../include/mm/pmm.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * ─── 全局变量 ───
 *
 * current_directory：
 *   当前正在使用的页目录指针（虚拟地址）。
 *   使用此指针而不是读取 CR3，是因为 CR3 存的是物理地址，
 *   而内核在身份映射下虚拟地址 == 物理地址，所以暂时无异。
 */

/** 当前活动的页目录指针（用于后续操作中的参照） */
static uint32_t* current_directory = NULL;

/**
 * 内核页目录。
 * __attribute__((aligned(4096))) 确保该数组起始地址为 4KB 对齐，
 * 因为 CR3 寄存器需要物理页基址（低 12 位为 0）。
 * 共 1024 个 32 位项 = 4KB，恰好占一页。
 */
static uint32_t kernel_page_directory[1024] __attribute__((aligned(4096)));

/* 注意：不再定义 kernel_first_page_table。
 * 旧代码里它是个从未被使用的静态数组，还曾被 vmm_destroy_directory
 * 误当成"内核静态页表"来比较（判据永远不成立，导致内核共享页表
 * 会被错误释放）。vmm_init 里的所有页表（包括第一张）都是 PMM
 * 动态分配的，共享页表的保护应比较 kernel_page_directory[0]。 */


/* ============================================================
 *  辅助函数
 * ============================================================ */

/**
 * 内存清零函数。
 * 逐字节将指定内存区域设为 0。
 * 用于初始化新分配的页表/页目录。
 */
static inline void zero_memory(void* ptr, size_t size) {
    unsigned char* p = (unsigned char*)ptr;
    while (size--) *p++ = 0;
}

/**
 * 设置页表/页目录项。
 *
 * 一个页表/页目录项（32位）的格式：
 *   位 31-12: 物理页基址（4KB 对齐，低 12 位必须为 0）
 *   位 11-0:  标志位（Present, R/W, U/S 等）
 *
 * @param entry      目标表项指针
 * @param phys_addr  物理地址（会被 & 0xFFFFF000 强制对齐）
 * @param flags      标志位（低 12 位，不含 Present，函数自动添加）
 */
static inline void set_entry(uint32_t* entry, uint32_t phys_addr, uint32_t flags) {
    /*
     * 构造最终的项值：
     *   (phys_addr & 0xFFFFF000) — 清除低 12 位，确保 4KB 对齐
     *   | (flags & 0xFFF)        — 写入标志位
     *   | PAGE_PRESENT           — 强制置 Present 位（CPU 需要）
     */
    *entry = (phys_addr & 0xFFFFF000) | (flags & 0xFFF) | PAGE_PRESENT;
}

/**
 * 清除页表/页目录项（置为 0 = 无效/不存在）。
 */
static inline void clear_entry(uint32_t* entry) {
    *entry = 0;
}

/**
 * 检查页表/页目录项的 Present 位（P 位，位 0）。
 * 如果 P=1，表示该项有效（对应的页表或物理页在内存中）。
 */
static inline bool is_present(uint32_t entry) {
    return (entry & PAGE_PRESENT) != 0;
}

/**
 * 从虚拟地址提取页目录索引（最高 10 位）。
 *  virt >> 22：右移 22 位，将位 22~31 移到最低 10 位
 *  范围：0~1023
 */
static inline uint32_t pd_index(uint32_t virt) { return virt >> 22; }

/**
 * 从虚拟地址提取页表索引（中间 10 位）。
 *  (virt >> 12) & 0x3FF：右移 12 位去掉页内偏移，再取低 10 位
 *  范围：0~1023
 */
static inline uint32_t pt_index(uint32_t virt) { return (virt >> 12) & 0x3FF; }

/**
 * 分配一个物理页并清零，作为新的页表使用。
 * 从 PMM 获取一页物理内存，清零全部 1024 个 PTE 项。
 *
 * @return 新页表的物理地址（作为指针使用），失败返回 NULL
 */
static uint32_t* allocate_pt(void) {
    uint32_t* pt = (uint32_t*)pmm_alloc_page();
    if (pt) {
        zero_memory(pt, 1024 * sizeof(uint32_t));
    }
    return pt;
}

/**
 * 释放一个页表占用的物理页。
 * 调用 PMM 的释放函数将页表所在的物理页归还。
 */
static void free_pt(uint32_t* pt) {
    if (pt) pmm_free_page((void*)pt);
}


/* ============================================================
 *  vmm_init — 虚拟内存初始化
 *
 *  这是整个虚拟内存系统的核心初始化过程。
 *
 *  做了什么：
 *    1. 清零内核页目录
 *    2. 计算需要多少个页表来映射所有物理内存
 *    3. 对每个页表：
 *       a. 从 PMM 分配一页物理内存
 *       b. 填充 1024 个 PTE，每个 PTE 映射一个 4KB 物理页
 *       c. 将页表地址和权限写入页目录的对应 PDE
 *    4. 加载页目录到 CR3，正式启用分页
 *
 *  映射关系示例（假设 8MB 物理内存）：
 *
 *     PD索引 0 (virt 0~4MB)   → PT[0]（包含 1024 个 PTE）
 *        PTE[0] → phys 0x00000000 (P|W)
 *        PTE[1] → phys 0x00001000 (P|W)
 *        ...
 *        PTE[1023] → phys 0x003FF000 (P|W)
 *
 *     PD索引 1 (virt 4~8MB)  → PT[1]（包含 1024 个 PTE）
 *        PTE[0] → phys 0x00400000 (P|W)
 *        PTE[1] → phys 0x00401000 (P|W)
 *        ...
 *        PTE[1023] → phys 0x007FF000 (P|W)
 *
 *     PD索引 2~1023 → 不存在（P=0），访问触发页故障 #PF
 *
 *  每个 PTE 的标志为 0x02 = PRESENT | WRITE。
 *  这意味着映射是可读写的，但仅内核特权级可访问（U/S=0）。
 * ============================================================ */
void vmm_init(void) {
    vga_write("[VMM] Initializing paging...\n");

    /* 清零页目录中所有 1024 个 PDE */
    zero_memory(kernel_page_directory, sizeof(kernel_page_directory));

    /* 查询物理内存总页数 */
    size_t total_pages = pmm_get_memory_size() / 4096;
    if (total_pages == 0) {
        vga_write("[VMM] ERROR: No memory detected!\n");
        return;
    }

    /*
     * 计算需要的页表数。
     * 每个页表覆盖 4MB（1024 页 × 4KB），所以：
     *   num_page_tables = ceil(total_pages / 1024)
     *
     * 最多 1024 个页表（因为页目录只有 1024 个槽位）。
     */
    uint32_t num_page_tables = (total_pages + 1023) / 1024;
    if (num_page_tables > 1024) num_page_tables = 1024; /* 最大 4GB */

    vga_write("[VMM] Mapping ");
    vga_write_hex(total_pages * 4096 / 1024);
    vga_write(" KB using ");
    vga_write_hex(num_page_tables);
    vga_write(" page tables.\n");

    /*
     * ─── 主循环：分配并填充页表，建立身份映射 ───
     *
     * 对每个页目录项索引 i：
     *   1. 分配一个新页表（物理页）
     *   2. 填充页表项：PTE[j] → 物理页地址 = i*4MB + j*4KB
     *   3. 设置页目录项：PDE[i] → 指向新页表
     */
    for (uint32_t i = 0; i < num_page_tables; i++) {

        /* 分配一个物理页作为页表 */
        uint32_t* pt = (uint32_t*)pmm_alloc_page();
        if (!pt) {
            vga_write("[VMM] ERROR: Failed to allocate page table!\n");
            return;
        }

        /* 清零新页表的所有 1024 个项 */
        zero_memory(pt, 1024 * sizeof(uint32_t));

        /*
         * 填充页表项：第 i 个页表对应的物理基地址 = i * 4MB
         * 因为：
         *   页目录索引 0 → 虚拟/物理 0x00000000~0x003FFFFF
         *   页目录索引 1 → 虚拟/物理 0x00400000~0x007FFFFF
         *   页目录索引 2 → 虚拟/物理 0x00800000~0x00BFFFFF
         *   依此类推...
         */
        uint32_t base_phys = i * 1024 * 4096;  /* = i * 0x400000 = i * 4MB */

        for (uint32_t j = 0; j < 1024; j++) {
            uint32_t phys = base_phys + j * 4096;

            /*
             * 超出实际物理内存大小的页表项不设置（保持为 0 = 不存在），
             * 这样访问未映射地址时会触发缺页异常（Page Fault）。
             */
            if (phys >= pmm_get_memory_size()) break;

            /*
             * 设置页表项：PRESENT | WRITE (0x03)
             * 但用 0x02 是因为 set_entry 会自动添加 PAGE_PRESENT，
             * 所以传入 flags=0x02 实际得到 PRESENT | WRITE。
             */
            set_entry(&pt[j], phys, 0x02); /* PRESENT | WRITE */
        }

        /*
         * 设置页目录项：
         *   页目录项 PDE[i] → 页表 pt 的物理地址
         *   标志：PRESENT | WRITE
         */
        set_entry(&kernel_page_directory[i], (uint32_t)pt, 0x02);
    }

    /*
     * ─── 加载页目录并启用分页 ───
     *
     * 将 kernel_page_directory 的物理地址写入 CR3 寄存器。
     * CR3 的第 12~31 位是页目录物理基址（低 12 位为 0 = 4KB 对齐）。
     *
     * 由于我们使用身份映射，CR3 写入后，EIP 中现有的指令地址
     * 仍然有效（虚拟地址 == 物理地址），分页平滑启用。
     */
    current_directory = kernel_page_directory;
    vmm_switch_directory(current_directory);

    vga_write("[VMM] Paging enabled with identity mapping over all physical memory.\n");
}


/* ============================================================
 *  vmm_create_directory — 创建新的页目录
 *
 *  用于创建用户进程的独立地址空间。
 *
 *  过程：
 *    1. 从 PMM 分配一页物理内存作为页目录（4KB 对齐）
 *    2. 清零全部 1024 个 PDE
 *    3. 复制内核的第一个 PDE（映射 0~4MB）到新目录
 *       —— 这样用户进程切换到该目录后仍能访问内核代码
 *
 *  注意：新页目录中只复制了内核的 PDE[0]（共享内核空间），
 *  其他 PDE 均为空，需要后续通过 vmm_map_page 填充。
 *
 *  @return 新页目录的物理地址（指针），失败返回 NULL
 * ============================================================ */
uint32_t* vmm_create_directory(void) {
    uint32_t* dir = (uint32_t*)pmm_alloc_page();
    if (!dir) return NULL;

    zero_memory(dir, 1024 * sizeof(uint32_t));

    /*
     * 复制内核页目录的第 0 项（PDE[0]）。
     * 这样新进程也能访问虚拟地址 0~4MB 的内核空间。
     * 这是最简单的"内核共享"策略——所有进程共享低 4MB 的映射。
     */
    dir[0] = kernel_page_directory[0];

    return dir;
}


/* ============================================================
 *  vmm_destroy_directory — 销毁页目录
 *
 *  释放一个页目录及其关联的所有非内核页表。
 *
 *  遍历页目录中所有 Present 的 PDE：
 *    1. 如果是内核第一个页表（静态分配），跳过
 *    2. 否则释放页表占用的物理页
 *  最后释放页目录自身的物理页。
 *
 *  注意：不会解除已释放页表所映射的虚拟地址上的引用。
 *  调用方必须确保没有其他进程在使用这些页表。
 *
 *  @param dir  要销毁的页目录指针
 * ============================================================ */
void vmm_destroy_directory(uint32_t* dir) {
    if (!dir) return;

    /* 内核共享的低 4MB 页表（PDE[0] 指向的那张），绝不能释放：
     * 它是 vmm_init 里 PMM 动态分配的、被所有目录共享的页表。
     * 旧实现拿静态数组 kernel_first_page_table 的地址做比较，
     * 但那个数组从未被使用过（死代码），判据永远不成立，
     * 一旦调用 destroy 就会把内核共享页表也释放掉 → 双重释放。
     * 正确做法：与内核页目录 PDE[0] 的物理地址（忽略标志位）比较。 */
    uint32_t kernel_pt0_phys = kernel_page_directory[0] & 0xFFFFF000;

    for (uint32_t i = 0; i < 1024; i++) {
        if (is_present(dir[i])) {
            /* 从 PDE 中提取页表物理地址。
             * PDE 格式：高 20 位 = 物理页基址，低 12 位 = 标志 */
            uint32_t pt_phys = dir[i] & 0xFFFFF000;

            /* 跳过内核共享页表（以及任何与内核相同的页表） */
            if (pt_phys == kernel_pt0_phys) continue;

            uint32_t* pt = (uint32_t*)pt_phys;
            free_pt(pt);
        }
    }

    /* 释放页目录本身占用的物理页 */
    pmm_free_page((void*)dir);
}


/* ============================================================
 *  vmm_map_page — 映射虚拟页到物理页
 *
 *  在指定页目录中建立虚拟地址→物理地址的映射关系。
 *
 *  完整流程：
 *    1. 从虚拟地址提取 PD 索引和 PT 索引
 *    2. 如果 PD 索引对应的页表不存在（P=0）：
 *       a. 分配一个新页表
 *       b. 填充 PDE（PRESENT | WRITE，加上 USER 如果需要）
 *    3. 如果页表已存在：
 *       a. 如果请求 USER 权限，给 PDE 添加 USER 位
 *    4. 检查目标 PTE 是否已被占用（已存在的映射不能覆盖）
 *    5. 设置 PTE：物理页地址 + 标志
 *    6. 刷新 TLB 中缓存的该页表项
 *
 *  @param dir        目标页目录
 *  @param virt_addr  虚拟地址（4KB 对齐会被自动强制）
 *  @param phys_addr  物理地址（4KB 对齐会被自动强制）
 *  @param flags      PTE 标志位（不含 PRESENT，函数自动添加）
 *  @return true=成功, false=失败（如内存不足或映射已存在）
 * ============================================================ */
bool vmm_map_page(uint32_t* dir, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    if (!dir) return false;

    /* 强制 4KB 对齐：清除低 12 位（页内偏移） */
    virt_addr &= 0xFFFFF000;
    phys_addr &= 0xFFFFF000;

    /* 分解虚拟地址 */
    uint32_t pd_idx = pd_index(virt_addr);   /* 页目录索引（0~1023） */
    uint32_t pt_idx = pt_index(virt_addr);   /* 页表索引（0~1023） */

    uint32_t* pt = NULL;

    if (!is_present(dir[pd_idx])) {
        /*
         * ---- 情况 A：页表不存在 ----
         *
         * PDE 的 P=0，表示该虚拟地址范围内还没有分配页表。
         * 需要：分配新页表 → 填充 PDE → 填充 PTE。
         */
        pt = allocate_pt();
        if (!pt) return false;

        /*
         * PDE 标志：PRESENT | WRITE 是基本要求。
         * 如果 PTE 请求 USER 权限（用户态可访问），
         * 则 PDE 也必须设置 USER 位，否则 CPU 在页表遍历时
         * 会在 PDE 级别拒绝访问。
         */
        uint32_t pde_flags = PAGE_PRESENT | PAGE_WRITE;
        if (flags & PAGE_USER) pde_flags |= PAGE_USER;

        /* 设置 PDE → 指向新页表 */
        set_entry(&dir[pd_idx], (uint32_t)pt, pde_flags);

        vga_write("[VMM] Allocated new page table, PDE flags=0x");
        vga_write_hex(pde_flags);
        vga_write("\n");

    } else {
        /*
         * ---- 情况 B：页表已存在 ----
         *
         * PDE 已存在且 P=1。
         * 从 PDE 中提取页表物理地址（高 20 位）。
         */
        if (flags & PAGE_USER) {
            /* ⚠️ 修复：共享内核页表（PDE[0] 指向的，所有进程目录共用）
             * 绝不能加 USER 位——否则整个低 4MB 内核代码/数据对用户态
             * 开放（可读可写，权限提升）。用户进程在 0-4MB 区域映射
             * 直接拒绝（该区域是内核保留区，用户程序应从 4MB 以上加载）。 */
            uint32_t pt_phys = dir[pd_idx] & 0xFFFFF000;
            uint32_t kernel_pt0_phys = kernel_page_directory[0] & 0xFFFFF000;
            if (pt_phys == kernel_pt0_phys) return false;

            /*
             * 如果请求用户级访问，但 PDE 还没有设置 USER 位，
             * 需要补上，否则用户态代码访问时会触发页故障。
             */
            dir[pd_idx] |= PAGE_USER;
            vga_write("[VMM] Added USER bit to existing PDE\n");
        }

        uint32_t pt_phys = dir[pd_idx] & 0xFFFFF000;
        pt = (uint32_t*)pt_phys;
    }

    /*
     * 安全检查：目标 PTE 已经存在（P=1）？
     * 如果是，返回失败，不覆盖已有映射。
     * 这防止了意外破坏已有页表导致的难以调试的错误。
     * 如果需要重新映射，必须先 unmap。
     */
    if (is_present(pt[pt_idx])) return false;

    /*
     * 设置页表项：物理地址 + 标志（自动加 PRESENT）。
     *
     * 现在 PTE 包含了完整的映射关系：
     *   虚拟地址 → PDE (present) → PTE (present) → 物理页
     * 当 CPU 访问此虚拟地址时，MMU 自动完成上述两级转换。
     */
    set_entry(&pt[pt_idx], phys_addr, flags | PAGE_PRESENT);

    /*
     * ─── TLB 刷新：INVLPG 指令 ───
     *
     * TLB（Translation Lookaside Buffer）是 CPU 内部的页表缓存。
     * 修改页表后，TLB 中可能还缓存着旧的映射。
     *
     * invlpg（INValidate Page）指令使指定虚拟地址的 TLB 项失效。
     * 内联汇编语法：
     *   "invlpg (%0)" — 其中 %0 是第一个操作数（virt_addr）
     *   : : "r"(virt_addr) — 将 virt_addr 存入寄存器作为输入
     *   : "memory" — 告知编译器内存可能被修改
     */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");

    return true;
}


/* ============================================================
 *  vmm_unmap_page — 解除虚拟页映射
 *
 *  将指定的虚拟地址映射关系移除（PTE 置 0）。
 *
 *  流程：
 *    1. 检查 PDE 是否存在（P=1）
 *    2. 检查 PTE 是否存在（P=1）
 *    3. 清除 PTE（置 0）
 *    4. 刷新 TLB
 *
 *  注意：本函数只会清除 PTE，不会释放物理页本身。
 *  物理页的释放需要调用方自行通过 PMM 处理。
 *
 *  @param dir        页目录指针
 *  @param virt_addr  要解除映射的虚拟地址
 *  @return true=成功, false=映射不存在或参数无效
 * ============================================================ */
bool vmm_unmap_page(uint32_t* dir, uint32_t virt_addr) {
    if (!dir) return false;

    virt_addr &= 0xFFFFF000;
    uint32_t pd_idx = pd_index(virt_addr);
    uint32_t pt_idx = pt_index(virt_addr);

    /* PDE 不存在，映射不可能存在 */
    if (!is_present(dir[pd_idx])) return false;

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & 0xFFFFF000);

    /* PTE 不存在，无需解映射 */
    if (!is_present(pt[pt_idx])) return false;

    /* 清除 PTE（置为 0） */
    clear_entry(&pt[pt_idx]);

    /* TLB 刷新 */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");

    return true;
}


/* ============================================================
 *  vmm_is_user_accessible — 用户地址校验（系统调用用）
 *
 *  遍历页目录/页表，检查指定虚拟地址所在的页：
 *    1. PDE 存在
 *    2. PTE 存在
 *    3. PTE 带 PAGE_USER 标志（ring 3 可访问）
 *  三者都满足才返回 true。
 * ============================================================ */
bool vmm_is_user_accessible(uint32_t virt_addr) {
    if (!current_directory) return false;

    uint32_t pd_idx = pd_index(virt_addr);
    uint32_t pt_idx = pt_index(virt_addr);

    /* PDE 不存在 */
    if (!is_present(current_directory[pd_idx])) return false;

    uint32_t* pt = (uint32_t*)(current_directory[pd_idx] & 0xFFFFF000);

    /* PTE 不存在或不是用户可访问页 */
    if (!is_present(pt[pt_idx])) return false;
    if (!(pt[pt_idx] & PAGE_USER)) return false;

    return true;
}

/* ============================================================
 *  vmm_get_phys_addr — 查询虚拟地址对应的物理地址
 *
 *  通过手动遍历页目录和页表，查询指定虚拟地址的物理地址。
 *  这模拟了 CPU MMU 的页表遍历过程。
 *
 *  返回值的组成：
 *    高位（4KB 对齐部分）= 从页表项获取的物理页基址
 *    低位（页内偏移）    = 直接来自虚拟地址的低 12 位
 *
 *  例：virt=0x12345678 映射到 phys=0x10000000
 *      返回 = 0x10000000 | (0x12345678 & 0xFFF) = 0x10000678
 *
 *  @param dir        页目录指针
 *  @param virt_addr  虚拟地址
 *  @return 物理地址（包含页内偏移），0 表示映射不存在
 * ============================================================ */
uint32_t vmm_get_phys_addr(uint32_t* dir, uint32_t virt_addr) {
    if (!dir) return 0;

    uint32_t pd_idx = pd_index(virt_addr);
    uint32_t pt_idx = pt_index(virt_addr);

    /* PDE 不存在 */
    if (!is_present(dir[pd_idx])) return 0;

    /* 获取页表指针（从 PDE 中提取物理地址） */
    uint32_t* pt = (uint32_t*)(dir[pd_idx] & 0xFFFFF000);

    /* PTE 不存在 */
    if (!is_present(pt[pt_idx])) return 0;

    /*
     * 读取物理页基址（PTE 的高 20 位），
     * 再加上虚拟地址的页内偏移（低 12 位）。
     */
    uint32_t phys = pt[pt_idx] & 0xFFFFF000;
    return phys | (virt_addr & 0xFFF);
}


/* ============================================================
 *  vmm_switch_directory — 切换页目录（地址空间切换）
 *
 *  这是进程切换的关键步骤。
 *  通过将新页目录的物理地址写入 CR3 寄存器，
 *  使 CPU 的 MMU 使用新的页表进行地址翻译。
 *
 *  CR3 写入的规格：
 *    位 31-12: 页目录物理地址（4KB 对齐）
 *    位 11-3:  保留（必须为 0）
 *    位 4-3:   PCD/PWT（缓存策略）
 *    位 2-0:   保留
 *
 *  写入 CR3 的效果：
 *    1. MMU 立即使用新页目录
 *    2. CPU 自动刷新整个 TLB（全部页表项缓存失效）
 *    3. 后续指令的虚拟地址按新页表进行翻译
 *
 *  内联汇编中的 "mov %0, %%cr3"：
 *    %0 是页目录地址（作为 32 位整数传入）
 *    执行后，CR3 指向新页目录，地址空间切换完成。
 *
 *  @param dir  新页目录指针
 * ============================================================ */
void vmm_switch_directory(uint32_t* dir) {
    if (!dir) return;
    current_directory = dir;
    /*
     * 将页目录指针写入 CR3。
     * CR3 = 页目录物理基址（必须 4KB 对齐）。
     * 由于身份映射，虚拟地址 == 物理地址。
     */
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)dir) : "memory");
}


/* ============================================================
 *  获取当前页目录指针
 *
 *  @return 当前使用的页目录的虚拟地址（或物理地址，由身份映射等价）
 * ============================================================ */
uint32_t* vmm_get_current_directory(void) {
    return current_directory;
}
