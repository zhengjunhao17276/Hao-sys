/*
 * vmm.h - 虚拟内存管理器接口（32 位两级分页）
 * 早期采用身份映射（虚拟地址 = 物理地址），为用户进程独立地址空间打基础。
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

/** 初始化分页：身份映射全物理内存并打开 CR0.PG */
void vmm_init(void);

/** 创建新页目录（复制内核全部 PDE 共享内核页表），失败返回 NULL */
uint32_t* vmm_create_directory(void);

/** 销毁页目录：释放非内核页表，再释放页目录自身 */
void vmm_destroy_directory(uint32_t* dir);

/** 映射 virt→phys（自动对齐 4KB，页表不存在会自动分配；映射已存在返回 false） */
bool vmm_map_page(uint32_t* dir, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);

/** 解除映射（不释放物理页） */
bool vmm_unmap_page(uint32_t* dir, uint32_t virt_addr);

/** 该页是否已映射且带 PAGE_USER。syscall 校验用户指针用，不能盲目解引用用户地址 */
bool vmm_is_user_accessible(uint32_t virt_addr);

/** 查虚拟地址对应的物理地址（含页内偏移），未映射返回 0 */
uint32_t vmm_get_phys_addr(uint32_t* dir, uint32_t virt_addr);

/** 切换页目录（进程上下文切换时用） */
void vmm_switch_directory(uint32_t* dir);

/** 当前页目录指针 */
uint32_t* vmm_get_current_directory(void);

#endif
