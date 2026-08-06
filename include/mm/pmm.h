/**
 * =========================================================================
 * pmm.h - 物理内存管理器接口
 *
 * PMM（Physical Memory Manager）负责跟踪物理内存页（4KB）的分配和释放。
 * HaoOS 使用最简单的位图（bitmap）方式管理物理内存：
 *
 *   - 位图中的每一位（bit）代表一个 4KB 物理页
 *   - bit=1 表示该页可用（空闲），bit=0 表示已分配
 *   - 位图本身放在内核代码的末尾（_end 之后），在启动时清零
 *
 * 内存检测流程：
 *   1. pmm_init() 从 multiboot_info_t 获取总内存大小
 *   2. 标记 1MB 以上区域为"可用"（低端 1MB 保留给 BIOS/内核）
 *   3. 如果有 MMAP 信息，进一步精细化标记
 *   4. 将内核自身占用的页标记为"已分配"
 *   5. 将位图自身占用的页也标记为"已分配"
 * =========================================================================
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/**
 * pmm_init - 初始化物理内存管理器
 * @info_addr: GRUB 传递的 multiboot_info_t 结构体物理地址
 *
 * 解析 Multiboot 信息，初始化位图，标记所有可用页。
 * 如果检测到 0 空闲页，会输出错误信息并进入死循环。
 */
void pmm_init(uint32_t info_addr);

/**
 * pmm_alloc_page - 分配一个 4KB 物理页
 * 返回值：物理地址（在启用分页后也是线性地址），失败返回 NULL
 *
 * 扫描位图找到第一个可用页，将其标记为已分配并返回其地址。
 * 不执行页面清零（调用者如有需要应自行清零）。
 */
void* pmm_alloc_page(void);

/**
 * pmm_alloc_contiguous - 分配 count 个物理连续的页（DMA 用，初始化阶段）
 */
void* pmm_alloc_contiguous(uint32_t count);

/**
 * pmm_free_page - 释放一个物理页
 * @addr: 要释放的物理页地址（必须是 4KB 对齐）
 */
void pmm_free_page(void* addr);

/**
 * pmm_get_memory_size - 获取可管理的总物理内存大小（字节）
 */
size_t pmm_get_memory_size(void);

/**
 * pmm_get_free_pages - 获取当前空闲页数
 */
size_t pmm_get_free_pages(void);

#endif
