/**
 * =========================================================================
 * vmm.h - 虚拟内存管理器接口（32 位分页）
 *
 * i386 架构使用两级页表结构实现虚拟地址到物理地址的转换：
 *
 *   虚拟地址（32 位）= [页目录索引 10 位] [页表索引 10 位] [页内偏移 12 位]
 *
 *   页目录（Page Directory, 4KB）共 1024 个页目录项（PDE），每个指向
 *   一个页表（Page Table, 4KB）。每个页表共 1024 个页表项（PTE），
 *   每个指向一个 4KB 物理页。
 *
 * 映射关系：一个页目录最多覆盖 1024 × 1024 × 4KB = 4GB 地址空间。
 *
 * CR3 寄存器保存当前页目录的物理地址。切换页目录即切换地址空间。
 *
 * HaoOS 当前采用"身份映射"（identity mapping）——虚拟地址 = 物理地址，
 * 全物理内存范围都映射。这是最简单的分页方案，为后续用户态进程的独立
 * 地址空间做基础。
 *
 * 页标志位说明：
 *   PAGE_PRESENT    - 页在物理内存中（否则触发缺页异常 #PF）
 *   PAGE_WRITE      - 可写（否则只读）
 *   PAGE_USER        - 用户态可访问（否则仅内核态可访问）
 *   PAGE_WRITETHROUGH - 写透缓存（禁用回写缓存）
 *   PAGE_CACHE_DISABLE - 禁用缓存
 *   PAGE_ACCESSED   - 已被访问（CPU 自动置位）
 *   PAGE_DIRTY      - 已被写入（CPU 自动置位）
 *   PAGE_GLOBAL     - 全局页（切换 CR3 时不刷新 TLB，需要 PGE 支持）
 * =========================================================================
 */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 页大小常量 */
#define PAGE_SIZE 4096

/* ---- 页表/页目录项标志位 ---- */
#define PAGE_PRESENT        (1 << 0)  /* 页在内存中 */
#define PAGE_WRITE          (1 << 1)  /* 可读写 */
#define PAGE_USER           (1 << 2)  /* 用户态（ring 3）可访问 */
#define PAGE_WRITETHROUGH   (1 << 3)  /* 写透缓存策略 */
#define PAGE_CACHE_DISABLE  (1 << 4)  /* 禁用缓存 */
#define PAGE_ACCESSED       (1 << 5)  /* CPU 在此页面访问时自动置位 */
#define PAGE_DIRTY          (1 << 6)  /* CPU 在此页面写入时自动置位 */
#define PAGE_GLOBAL         (1 << 7)  /* 全局页面（切换 CR3 时 TLB 不清除） */

/* ---- 函数声明 ---- */

/**
 * vmm_init - 初始化分页系统
 *
 * 分配页表，建立覆盖所有物理内存的身份映射：
 *   虚拟地址 = 物理地址（identity mapping）
 * 设置 CR0 的 PG 位以启用分页，加载页目录到 CR3。
 * 此时内核运行在分页模式下。
 */
void vmm_init(void);

/**
 * vmm_create_directory - 创建一个新的空页目录
 * 返回值：页目录指针（可作为 CR3 值），失败返回 NULL
 *
 * 新页目录在索引 0 处复制了内核页目录的首个页表项，
 * 这样新进程也能访问内核代码和数据。
 */
uint32_t* vmm_create_directory(void);

/**
 * vmm_destroy_directory - 销毁页目录及下属页表
 * @dir: 要销毁的页目录指针
 *
 * 遍历所有页目录项，释放非内核的页表物理页，最后释放页目录自身。
 */
void vmm_destroy_directory(uint32_t* dir);

/**
 * vmm_map_page - 将虚拟地址映射到物理地址
 * @dir:       页目录指针
 * @virt_addr: 虚拟地址（自动对齐到 4KB 边界）
 * @phys_addr: 物理地址（自动对齐到 4KB 边界）
 * @flags:     页标志位（PAGE_PRESENT, PAGE_WRITE, PAGE_USER 等组合）
 * 返回 true 表示映射成功
 *
 * 如果所需页表不存在，会调用 pmm_alloc_page 自动分配新页表。
 * 映射成功后执行 invlpg 指令刷新 TLB。
 */
bool vmm_map_page(uint32_t* dir, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);

/**
 * vmm_unmap_page - 解除虚拟地址的映射
 * @dir:       页目录指针
 * @virt_addr: 要解除映射的虚拟地址
 * 返回 true 表示解映射成功
 */
bool vmm_unmap_page(uint32_t* dir, uint32_t virt_addr);

/**
 * vmm_is_user_accessible - 检查虚拟地址是否被映射为"用户态可访问"
 * @virt_addr: 虚拟地址（页内偏移不影响判断）
 * 返回 true = 该页已映射且带 PAGE_USER 标志（ring 3 可访问）
 *
 * 用于系统调用的指针校验：内核不能盲目解引用用户传来的地址，
 * 否则用户可以让内核把数据写进任意内存（权限提升/崩溃）。
 */
bool vmm_is_user_accessible(uint32_t virt_addr);

/**
 * vmm_get_phys_addr - 查询虚拟地址对应的物理地址
 * @dir:       页目录指针
 * @virt_addr: 虚拟地址
 * 返回值：物理地址（含页内偏移），如果未映射则返回 0
 */
uint32_t vmm_get_phys_addr(uint32_t* dir, uint32_t virt_addr);

/**
 * vmm_switch_directory - 切换页目录（切换地址空间）
 * @dir: 目标页目录指针
 *
 * 将 dir 加载到 CR3 寄存器，使新的页表生效。
 * 用于进程上下文切换时切换到目标进程的地址空间。
 */
void vmm_switch_directory(uint32_t* dir);

/**
 * vmm_get_current_directory - 获取当前使用的页目录指针
 */
uint32_t* vmm_get_current_directory(void);

#endif
