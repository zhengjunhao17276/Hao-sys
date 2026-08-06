/*
 * ============================================================
 * pmm.c — 物理内存管理器 (Physical Memory Manager)
 * ============================================================
 *
 * 本文件实现了基于位图（Bitmap）的物理内存页分配与回收。
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │  核心数据结构：位图                                      │
 * │                                                         │
 * │  物理内存被划分为 4KB 大小的"页"（Page），每页对应位图   │
 * │  中的一个 bit。                                         │
 * │                                                         │
 * │    bit = 1  → 该页空闲（可用）                          │
 * │    bit = 0  → 该页已占用（不可用）                      │
 * │                                                         │
 * │  物理地址 ↔ 位图索引转换：                              │
 * │    索引 = 地址 / 4096   （addr_to_index）                │
 * │    地址 = 索引 * 4096   （index_to_addr）                │
 * │                                                         │
 * │  例：物理地址 0x100000 (1MB) → 索引 = 256               │
 * │      物理地址 0x200000 (2MB) → 索引 = 512               │
 * └─────────────────────────────────────────────────────────┘
 *
 * 初始化流程（在 pmm_init 中执行）：
 *   1. 从 Multiboot 信息获取总内存大小
 *   2. 在位图尾部分配位图自身所需空间
 *   3. 标记所有物理页为"可用"（bit = 1）
 *   4. 标记不可用区域为"占用"（bit = 0）：
 *      - 0~1MB 区域（硬件/BIOS/内核使用）
 *      - 内核代码段驻留区域（_end 以内）
 *      - 位图自身所占页面
 *   5. 统计最终空闲页数
 *
 * ============================================================
 */

#include "../include/mm/pmm.h"
#include "../include/mm/multiboot.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * 链接器符号 _end：内核镜像的结束地址。
 * 由链接脚本定义，表示内核代码+数据段的末尾。
 * 位图就放置在此地址之后（紧挨着内核）。
 */
extern uint8_t _end[];

/* ---- 全局变量 ---- */

/** 位图基指针（指向位图数据在物理内存中的位置） */
static uint8_t *bitmap = NULL;

/** 物理内存总页数 = total_memory / 4096 */
static size_t total_pages = 0;

/** 当前空闲页数（用于快速查询，避免遍历位图） */
static size_t free_pages = 0;

/** 物理内存总大小（字节） */
static size_t total_memory = 0;


/* ============================================================
 *  地址 ↔ 位图索引 转换函数
 *
 *  由于页大小固定为 4096 字节（0x1000），
 *  地址和索引之间的转换就是简单的乘除关系。
 *  inline 关键字提示编译器直接在调用处展开以提升性能。
 * ============================================================ */

/**
 * 物理地址 → 位图索引
 *   addr / 4096 = addr >> 12
 *   例：0x100000 / 0x1000 = 256（第 256 位）
 */
static inline size_t addr_to_index(uintptr_t addr) {
    return addr / 4096;
}

/**
 * 位图索引 → 物理地址
 *   例：索引 256 × 4096 = 0x100000
 */
static inline uintptr_t index_to_addr(size_t idx) {
    return idx * 4096;
}


/* ============================================================
 *  位图位操作
 *
 *  位图以字节数组形式存储，每个字节包含 8 页的状态。
 *
 *     位图字节数组：
 *     bitmap[0]  bitmap[1]  bitmap[2]  ...
 *     [b7..b0]   [b7..b0]   [b7..b0]   ...
 *
 *     索引 i 对应：
 *       字节 = bitmap[i / 8]
 *       位   = i % 8（第 0 位是 LSB，第 7 位是 MSB）
 *
 *     所以：
 *       get_page_bit(i) → (bitmap[i/8] >> (i%8)) & 1
 *       set_page_bit(i, 1) → bitmap[i/8] |= (1 << (i%8))
 *       set_page_bit(i, 0) → bitmap[i/8] &= ~(1 << (i%8))
 * ============================================================ */

/**
 * 读取第 idx 页的位状态。
 * 返回 true = 空闲，false = 已占用。
 */
static inline bool get_page_bit(size_t idx) {
    return (bitmap[idx / 8] >> (idx % 8)) & 1;
}

/**
 * 设置第 idx 页的位状态。
 * @param available  true=标记为空闲, false=标记为已占用
 */
static inline void set_page_bit(size_t idx, bool available) {
    if (available)
        /* 置 1：使用 OR 操作，将指定位设为 1 而不影响其他位 */
        bitmap[idx / 8] |= (1 << (idx % 8));
    else
        /* 清 0：使用 AND 取反操作，将指定位设为 0 */
        bitmap[idx / 8] &= ~(1 << (idx % 8));
}


/* ============================================================
 *  标记内存区域
 *
 *  对 [start, start+length) 范围内的所有物理页设置位图标记。
 *  这是整个内存管理器最核心的辅助函数。
 *
 *  注意边界处理：
 *    - length=0 时直接返回
 *    - 计算 start-1 是为了让 end_idx 正确地覆盖最后一个页
 *      例：start=0x100000, length=0x1000（恰好一页）
 *          起始索引 = 0x100000/4096 = 256
 *          结束索引 = (0x100000+0x1000-1)/4096 = (0x100FFF)/4096 = 256
 *          只标记了第 256 页，正确。
 * ============================================================ */
static void mark_region(uintptr_t start, size_t length, bool available) {
    if (length == 0) return;

    size_t start_idx = addr_to_index(start);
    size_t end_idx   = addr_to_index(start + length - 1);

    /*
     * 遍历 [start_idx, end_idx] 范围内的所有页。
     * 添加 i < total_pages 保护，防止越界标记。
     */
    for (size_t i = start_idx; i <= end_idx && i < total_pages; i++) {
        set_page_bit(i, available);
    }
}


/* ============================================================
 *  物理内存管理器初始化
 *
 *  这是启动早期（进入保护模式、初始化 VGA 后）调用的关键初始化函数。
 *
 *  参数：
 *    info_addr — Multiboot 信息结构的物理地址
 *                （由引导程序（如 GRUB）提供）
 *
 *  Multiboot 信息结构包含：
 *    flags      ：标志位，指示哪些字段有效
 *    mem_lower  ：低端内存大小（KB），通常 = 640 KB
 *    mem_upper  ：高端内存大小（KB），即 1MB 以上的内存
 *    mmap_addr  ：内存映射基地址（可选，更详细的内存布局）
 *    mmap_length：内存映射大小
 *
 *  内存布局示例（假设总内存 64MB）：
 *    ┌─────────────┐ 0x00000000
 *    │  BIOS/硬件   │ ← 低端 1MB 保留
 *    ├─────────────┤ 0x00100000 (1MB)
 *    │  内核代码段  │ ← _end 之前
 *    ├─────────────┤ ← _end (内核结束)
 *    │  位图数组    │ ← 位图自身
 *    ├─────────────┤
 *    │  可用内存    │ ← 可分配
 *    ├─────────────┤
 *    │  ...        │
 *    └─────────────┘ 0x04000000 (64MB)
 * ============================================================ */
void pmm_init(uint32_t info_addr) {
    multiboot_info_t *info = (multiboot_info_t*)info_addr;

    vga_write("[PMM] Initializing physical memory manager...\n");

    vga_write("[PMM] Multiboot flags: 0x");
    vga_write_hex(info->flags);
    vga_write("\n");

    /*
     * ─── 第一步：获取物理内存总量 ───
     *
     * Multiboot 协议提供两种获取内存信息的方式：
     *   方式1（基础）：info->mem_lower + info->mem_upper
     *      - mem_lower = 1MB 以下可用内存（通常 640KB）
     *      - mem_upper = 1MB 以上可用内存
     *   方式2（详细）：info->mmap（内存映射，提供完整布局）
     *
     * 我们先用方式1获取总量，再用方式2补充具体的可用区域。
     */
    uint32_t mem_lower = info->mem_lower;   /* 单位：KB */
    uint32_t mem_upper = info->mem_upper;   /* 单位：KB */
    vga_write("[PMM] mem_lower: ");
    vga_write_hex(mem_lower);
    vga_write(" KB, mem_upper: ");
    vga_write_hex(mem_upper);
    vga_write(" KB\n");

    uint32_t total_kb = mem_lower + mem_upper;
    if (total_kb == 0) {
        /*
         * 容错处理：如果 Multiboot 信息中没有内存数据，
         * 假设一个保守值（16MB）以避免后续除零错误。
         */
        vga_write("[PMM] WARNING: mem_lower+mem_upper is 0, assuming 16MB.\n");
        total_kb = 16 * 1024;
    }

    total_memory = total_kb * 1024;          /* KB → 字节 */
    total_pages  = total_memory / 4096;      /* 总页数 */

    vga_write("[PMM] Total memory (from mem_lower+mem_upper): ");
    vga_write_hex(total_kb);
    vga_write(" KB\n");


    /*
     * ─── 第二步：分配位图空间 ───
     *
     * 位图大小 = ceil(total_pages / 8) 字节
     * 因为每页用 1 bit，8 页 = 1 字节。
     *
     * 例：64MB = 65536 页 → 位图 = 65536/8 = 8192 字节 ≈ 2 页
     *
     * 位图放置在内核结束地址（_end）处，紧挨着内核镜像。
     * 这是可行的，因为此时分页尚未启用，我们在物理地址空间直接操作。
     */
    size_t bitmap_size = (total_pages + 7) / 8;
    bitmap = (uint8_t*)_end;

    /* 将位图区域全部清零（初始化为"全部占用"状态） */
    for (size_t i = 0; i < bitmap_size; i++) bitmap[i] = 0;


    /*
     * ─── 第三步：标记可用内存 ───
     *
     * 策略说明：
     *   先标记 1MB 以上全部为可用（预留低端 1MB），
     *   然后逐步标记"不可用"区域。
     *
     *   为什么跳过低端 1MB？
     *     0x00000000 ~ 0x0009FFFF  : BIOS 数据区、IVT、BDA
     *     0x000A0000 ~ 0x000FFFFF  : VGA 显存、BIOS ROM、扩展 BIOS
     *     0x00100000               : 内核加载地址
     */

    /* 1MB 以上 → 全部标记为可用（bit = 1） */
    mark_region(0x100000, total_memory - 0x100000, true);

    /*
     * ─── 第四步：处理 MMAP（可选）───
     *
     * Multiboot 内存映射（MMAP）提供更精确的内存布局，
     * 如哪些低端区域实际可用，而不仅仅是 1MB 以上。
     *
     * 如果引导程序提供了 MMAP，遍历它并额外标记可用区域。
     *
     * 什么情况下有用？
     *   - 某些机器在低端 1MB 内有可用空间（如 0x10000 附近）
     *   - MMAP 能够准确识别这些"空隙"
     */
    if (info->flags & MULTIBOOT_MMAP) {
        vga_write("[PMM] Also processing MMAP for additional regions.\n");
        uint8_t *mmap_ptr = (uint8_t*)info->mmap_addr;
        uint8_t *mmap_end = mmap_ptr + info->mmap_length;

        /*
         * MMAP 中每个条目是一个 multiboot_mmap_entry_t 结构：
         *   size       ：本条大小（含自身）
         *   base_addr  ：区域基地址（64位）
         *   length     ：区域长度（64位）
         *   type       ：1=可用, 2=保留, 3=ACPI可回收, 4=ACPI NVS
         * 条目在 mmap_addr 处连续排列，每个条目长度由本条目中的 size+4 决定。
         */
        while (mmap_ptr < mmap_end) {
            multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t*)mmap_ptr;

            /* type=1 表示该区域可用 */
            if (entry->type == MULTIBOOT_MMAP_AVAILABLE) {
                uint64_t base = entry->base_addr_low;
                uint64_t length = entry->length_low;

                /* 防止 64 位截断和无效范围 */
                if (base < 0xFFFFFFFF && length > 0) {
                    mark_region((uintptr_t)base, (size_t)length, true);
                }
            } else {
                /* ⚠️ 修复：保留/ACPI/坏内存区域必须标为“不可用”。
                 * 之前只处理 type=1，而步骤 3 已把 1MB 以上全部标为可用，
                 * 导致 MMAP 里的保留区（ACPI 表、NVS、坏内存等）漏成
                 * 可分配页——QEMU 下碰巧不炸，真机上可能把 ACPI 内存
                 * 分给用户程序（写坏 ACPI 表 → 电源管理/关机异常）。 */
                uint64_t base = entry->base_addr_low;
                uint64_t length = entry->length_low;

                if (base < 0xFFFFFFFF && length > 0) {
                    mark_region((uintptr_t)base, (size_t)length, false);
                }
            }

            /* 推进到下一条目：当前地址 + size + 4（size 字段本身占4字节） */
            mmap_ptr += entry->size + 4;
        }
    }


    /*
     * ─── 第五步：统计当前空闲页 ───
     *
     * 在标记"不可用"区域之前，先遍历位图统计空闲页数。
     * 这仅为调试输出，后续还会修正。
     */
    free_pages = 0;
    for (size_t i = 0; i < total_pages; i++) {
        if (get_page_bit(i)) free_pages++;
    }
    vga_write("[PMM] Free pages (before reserving kernel): ");
    vga_write_hex(free_pages);
    vga_write("\n");


    /*
     * ─── 第六步：标记内核代码段为"已占用" ───
     *
     * 内核从 1MB (0x100000) 开始加载，到 _end 地址结束。
     * 这段物理内存已被内核代码和数据占用，不能分配。
     *
     * 遍历 [kernel_start, kernel_end] 对应的所有位图位，
     * 如果当前是可用状态（=1），改为已占用（=0）并减少空闲计数。
     */
    size_t kernel_start_idx = addr_to_index(0x100000);
    size_t kernel_end_idx   = addr_to_index((uintptr_t)_end - 1);

    for (size_t i = kernel_start_idx; i <= kernel_end_idx && i < total_pages; i++) {
        if (get_page_bit(i)) {
            set_page_bit(i, false);   /* 标记为已占用 */
            free_pages--;
        }
    }


    /*
     * ─── 第七步：标记位图自身为"已占用" ───
     *
     * 位图数组位于 _end 地址处，占用 bitmap_size 字节，
     * 跨越若干物理页。这些页也不能分配，否则会损坏位图。
     *
     * 注意：这是"先有鸡先有蛋"的问题——
     *       位图自身占用的页必须由位图自身来记录。
     */
    size_t bitmap_start_idx = addr_to_index((uintptr_t)bitmap);
    size_t bitmap_end_idx   = addr_to_index((uintptr_t)bitmap + bitmap_size - 1);

    for (size_t i = bitmap_start_idx; i <= bitmap_end_idx && i < total_pages; i++) {
        if (get_page_bit(i)) {
            set_page_bit(i, false);   /* 标记为已占用 */
            free_pages--;
        }
    }

    vga_write("[PMM] Final free pages: ");
    vga_write_hex(free_pages);
    vga_write("\n");

    /*
     * 安全检查：如果没有任何空闲页可用，系统无法继续运行。
     * 此时打印致命错误并停机（HLT 循环）。
     */
    if (free_pages == 0) {
        vga_write("[PMM] FATAL: No free pages available.\n");
        while (1) __asm__ volatile ("hlt");
    }
}


/* ============================================================
 *  分配一页物理内存
 *
 *  从位图中线性扫描，找到第一个空闲的物理页，
 *  将其标记为已占用并返回物理地址。
 *
 *  算法：
 *    遍历位图 → 找到 bit=1 的页 → 设为 0 → 返回对应地址
 *
 *  线性搜索的不足：
 *    时间复杂度 O(n)，对于大量分配效率较低。
 *    优化方案（如栈式空闲链表）留给后续改进。
 *
 *  @return 物理页基地址（0x1000 对齐），失败返回 NULL
 * ============================================================ */
void* pmm_alloc_page(void) {
    for (size_t i = 0; i < total_pages; i++) {
        if (get_page_bit(i)) {
            /* 找到空闲页 → 标记为已占用 */
            set_page_bit(i, false);
            free_pages--;
            return (void*)index_to_addr(i);
        }
    }
    /* 没有空闲页可用 */
    return NULL;
}


/* ============================================================
 *  释放一页物理内存
 *
 *  将指定物理地址对应的页标记为"空闲"。
 *
 *  安全性检查：
 *    - 地址超出总页数范围则忽略（容错）
 *    - 如果该页已经是空闲状态，不重复增加 free_pages
 *      （防止引用计数错误）
 *
 *  @param addr  要释放的物理页地址（4KB 对齐）
 * ============================================================ */
void pmm_free_page(void* addr) {
    uintptr_t paddr = (uintptr_t)addr;
    size_t idx = addr_to_index(paddr);

    /* 越界检查 */
    if (idx >= total_pages) return;

    /* 仅当该页当前为"已占用"时才释放，避免重复释放 */
    if (!get_page_bit(idx)) {
        set_page_bit(idx, true);   /* 标记为空闲 */
        free_pages++;
    }
}


/* ============================================================
 *  查询物理内存总大小
 *  @return 总字节数
 * ============================================================ */
size_t pmm_get_memory_size(void) {
    return total_memory;
}


/* ============================================================
 *  查询当前空闲页数
 *  @return 空闲页数量
 * ============================================================ */
size_t pmm_get_free_pages(void) {
    return free_pages;
}
