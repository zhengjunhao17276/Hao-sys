/*
 * pmm.h - 物理内存管理器接口
 * 位图 + 空闲页链表跟踪 4KB 物理页；位图放在 _end 之后。
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/** 从 Multiboot 信息初始化 PMM（0 空闲页会停机） */
void pmm_init(uint32_t info_addr);

/** 分配一页（不清零），失败返回 NULL */
void* pmm_alloc_page(void);

/** 分配 count 个物理连续的页（DMA 用，仅初始化阶段调用） */
void* pmm_alloc_contiguous(uint32_t count);

/** 释放一页（地址须 4KB 对齐） */
void pmm_free_page(void* addr);

/** 物理内存总大小（字节） */
size_t pmm_get_memory_size(void);

/** 当前空闲页数 */
size_t pmm_get_free_pages(void);

#endif
