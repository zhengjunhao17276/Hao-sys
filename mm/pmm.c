/*
 * pmm.c - 物理内存管理器
 * 位图 + 空闲页链表管理 4KB 物理页的分配与回收。
 */

#include "../include/mm/pmm.h"
#include "../include/mm/multiboot.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>

/* 内核镜像结束地址（链接脚本定义），位图就放在它后面 */
extern uint8_t _end[];

/* 全局变量 */

static uint8_t *bitmap = NULL;

static size_t total_pages = 0;

static size_t free_pages = 0;

static size_t total_memory = 0;

/* ⚠️ 架构升级：空闲页链表实现 O(1) 分配/释放。
 * 空闲页里第一个 4 字节存下一项页索引（页空闲时内容无意义），不占额外内存；
 * 位图仍保留做状态跟踪和重复释放检测。初始化时一次性遍历位图建链。 */
static uint32_t free_list_head = (uint32_t)-1;   /* -1 = 空链表 */


static inline size_t addr_to_index(uintptr_t addr) {
    return addr / 4096;
}

static inline uintptr_t index_to_addr(size_t idx) {
    return idx * 4096;
}


static inline bool get_page_bit(size_t idx) {
    return (bitmap[idx / 8] >> (idx % 8)) & 1;
}

static inline void set_page_bit(size_t idx, bool available) {
    if (available)
        bitmap[idx / 8] |= (1 << (idx % 8));
    else
        bitmap[idx / 8] &= ~(1 << (idx % 8));
}


/* 标记 [start, start+length) 内的所有页。
 * 末索引用 start+length-1 算，正好盖住最后一页；i < total_pages 防越界。 */
static void mark_region(uintptr_t start, size_t length, bool available) {
    if (length == 0) return;

    size_t start_idx = addr_to_index(start);
    size_t end_idx   = addr_to_index(start + length - 1);

    for (size_t i = start_idx; i <= end_idx && i < total_pages; i++) {
        set_page_bit(i, available);
    }
}


/* 初始化 PMM：取内存总量，建位图，标记可用/保留区，最后把空闲页挂链表 */
void pmm_init(uint32_t info_addr) {
    multiboot_info_t *info = (multiboot_info_t*)info_addr;

    /* 先用 mem_lower+mem_upper 拿总量，MMAP 后面再细化 */
    uint32_t mem_lower = info->mem_lower;   /* 单位：KB */
    uint32_t mem_upper = info->mem_upper;   /* 单位：KB */

    uint32_t total_kb = mem_lower + mem_upper;
    if (total_kb == 0) {
        /* 拿不到内存信息就给个保守值，免得后面除零 */
        vga_write("[PMM] WARNING: mem_lower+mem_upper is 0, assuming 16MB.\n");
        total_kb = 16 * 1024;
    }

    total_memory = total_kb * 1024;          /* KB → 字节 */
    total_pages  = total_memory / 4096;      /* 总页数 */
    vga_write("[PMM] Memory: ");
    vga_write_hex(total_kb);
    vga_write(" KB\n");


    /* 位图放 _end 处；还没开分页，直接写物理地址就行 */
    size_t bitmap_size = (total_pages + 7) / 8;
    bitmap = (uint8_t*)_end;

    /* 清零 = 默认全部占用 */
    for (size_t i = 0; i < bitmap_size; i++) bitmap[i] = 0;


    /* 低端 1MB 是 IVT/BDA/VGA 显存/BIOS ROM 的地盘，不能动；
     * 先假设 1MB 以上全可用，后面再按 MMAP 和内核占用往回抠 */
    mark_region(0x100000, total_memory - 0x100000, true);

    /* MMAP 能标出低端 1MB 里的可用空隙，有就给补上 */
    if (info->flags & MULTIBOOT_MMAP) {
        uint8_t *mmap_ptr = (uint8_t*)info->mmap_addr;
        uint8_t *mmap_end = mmap_ptr + info->mmap_length;

        /* 条目连续排列，步长 = size + 4；type=1 才是可用区域 */
        while (mmap_ptr < mmap_end) {
            multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t*)mmap_ptr;

            if (entry->type == MULTIBOOT_MMAP_AVAILABLE) {
                uint64_t base = entry->base_addr_low;
                uint64_t length = entry->length_low;

                if (base < 0xFFFFFFFF && length > 0) {
                    mark_region((uintptr_t)base, (size_t)length, true);
                }
            } else {
                /* ⚠️ 修复：保留/ACPI/坏内存必须标为不可用。
                 * 之前只处理 type=1，而 1MB 以上已被默认标为可用，
                 * 保留区会漏成可分配页——QEMU 下碰巧不炸，真机上
                 * 可能把 ACPI 内存分给用户程序，写坏 ACPI 表。 */
                uint64_t base = entry->base_addr_low;
                uint64_t length = entry->length_low;

                if (base < 0xFFFFFFFF && length > 0) {
                    mark_region((uintptr_t)base, (size_t)length, false);
                }
            }

            mmap_ptr += entry->size + 4;
        }
    }


    /* 统计空闲页（最终打印 + 无页可用的 FATAL 检查） */
    free_pages = 0;
    for (size_t i = 0; i < total_pages; i++) {
        if (get_page_bit(i)) free_pages++;
    }

    /* 内核镜像 [1MB, _end) 这段不能分出去 */
    size_t kernel_start_idx = addr_to_index(0x100000);
    size_t kernel_end_idx   = addr_to_index((uintptr_t)_end - 1);

    for (size_t i = kernel_start_idx; i <= kernel_end_idx && i < total_pages; i++) {
        if (get_page_bit(i)) {
            set_page_bit(i, false);
            free_pages--;
        }
    }


    /* 位图自己占的页也得标占用，不然分配时会把位图覆盖掉 */
    size_t bitmap_start_idx = addr_to_index((uintptr_t)bitmap);
    size_t bitmap_end_idx   = addr_to_index((uintptr_t)bitmap + bitmap_size - 1);

    for (size_t i = bitmap_start_idx; i <= bitmap_end_idx && i < total_pages; i++) {
        if (get_page_bit(i)) {
            set_page_bit(i, false);
            free_pages--;
        }
    }

    vga_write("[PMM] Free pages: ");
    vga_write_hex(free_pages);
    vga_write("\n");

    /* 一个空闲页都没有就别跑了，停机 */
    if (free_pages == 0) {
        vga_write("[PMM] FATAL: No free pages available.\n");
        while (1) __asm__ volatile ("hlt");
    }

    /* ⚠️ 架构升级：遍历位图一次性建链，此后 alloc/free 都是 O(1) */
    free_list_head = (uint32_t)-1;
    for (size_t i = 0; i < total_pages; i++) {
        if (get_page_bit(i)) {
            *(uint32_t*)index_to_addr(i) = free_list_head;
            free_list_head = (uint32_t)i;
        }
    }
}


void* pmm_alloc_page(void) {
    /* ⚠️ 从链表头取页，替代旧的线性扫描位图 */
    if (free_list_head == (uint32_t)-1) return NULL;
    uint32_t idx = free_list_head;
    free_list_head = *(uint32_t*)index_to_addr(idx);
    set_page_bit(idx, false);
    free_pages--;
    return (void*)index_to_addr(idx);
}

/* 分配 count 个物理连续的页（DMA 用）。
 * ⚠️ 链表分配不保证连续，这里保留线性扫描；只在初始化阶段调用，开销可接受。 */
void* pmm_alloc_contiguous(uint32_t count) {
    if (count == 0) return NULL;
    for (size_t i = 0; i < total_pages; i++) {
        if (!get_page_bit(i)) continue;
        uint32_t j;
        for (j = 1; j < count && i + j < total_pages; j++) {
            if (!get_page_bit(i + j)) break;
        }
        if (j == count) {
            /* 单链表只能遍历到目标页再逐个摘 */
            for (uint32_t k = 0; k < count; k++) {
                uint32_t prev = (uint32_t)-1;
                uint32_t cur = free_list_head;
                while (cur != (uint32_t)-1 && cur != i + k) {
                    prev = cur;
                    cur = *(uint32_t*)index_to_addr(cur);
                }
                if (cur == i + k) {
                    if (prev == (uint32_t)-1) free_list_head = *(uint32_t*)index_to_addr(cur);
                    else *(uint32_t*)index_to_addr(prev) = *(uint32_t*)index_to_addr(cur);
                }
                set_page_bit(i + k, false);
                free_pages--;
            }
            return (void*)index_to_addr(i);
        }
        i += j;   /* 跳过不连续的部分 */
    }
    return NULL;
}


/* 释放一页。越界直接忽略；重复释放靠位图挡掉，不会重复计数。 */
void pmm_free_page(void* addr) {
    uintptr_t paddr = (uintptr_t)addr;
    size_t idx = addr_to_index(paddr);

    if (idx >= total_pages) return;

    /* 只有占用中的页才释放，重复释放直接忽略 */
    if (!get_page_bit(idx)) {
        set_page_bit(idx, true);
        /* ⚠️ 头插回空闲链表 */
        *(uint32_t*)addr = free_list_head;
        free_list_head = (uint32_t)idx;
        free_pages++;
    }
}


size_t pmm_get_memory_size(void) {
    return total_memory;
}


size_t pmm_get_free_pages(void) {
    return free_pages;
}
