/**
 * =========================================================================
 * multiboot.h - Multiboot 信息结构定义
 *
 * Multiboot 是一个开放的操作系统引导规范，由 GRUB 等引导加载程序实现。
 * 当 GRUB 加载 HaoOS 时，会通过 ebx 寄存器传递一个 multiboot_info_t
 * 结构体的物理地址，内核通过解析这个结构体了解硬件配置。
 *
 * 信息包含：
 *   - 内存布局（低位内存 640KB、高位内存从 1MB 开始）
 *   - 内存映射（MMAP），详细描述哪些物理地址范围可用/保留
 *   - 命令行参数、模块信息
 *   - 引导加载器名称
 *   - VBE 显示模式信息（可选）
 *
 * 标志位（flags 字段的位掩码）：
 *   内核首先检查 info->flags 的哪些位被置位，确定哪些字段有效。
 *   例如 bit 6（0x40）表示 mmap_addr/mmap_length 有效。
 * =========================================================================
 */

#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* ---- Multiboot 信息标志位 ---- */
#define MULTIBOOT_MEMORY_INFO     0x01    /* bit0: mem_lower/mem_upper 有效 */
#define MULTIBOOT_BOOT_DEVICE     0x02    /* bit1: boot_device 有效 */
#define MULTIBOOT_CMDLINE         0x04    /* bit2: cmdline 有效 */
#define MULTIBOOT_MODS            0x08    /* bit3: mods 有效 */
#define MULTIBOOT_AOUT_SYMS       0x10    /* bit4: a.out 符号表有效 */
#define MULTIBOOT_ELF_SYMS        0x20    /* bit5: ELF 符号表有效 */
#define MULTIBOOT_MMAP            0x40    /* bit6: 内存映射（MMAP）有效 */
#define MULTIBOOT_DRIVES          0x80    /* bit7: 驱动器信息有效 */
#define MULTIBOOT_CONFIG          0x100   /* bit8: 配置文件有效 */
#define MULTIBOOT_LOADER_NAME     0x200   /* bit9: 引导器名称有效 */
#define MULTIBOOT_APM_TABLE       0x400   /* bit10: APM 表有效 */
#define MULTIBOOT_VIDEO_INFO      0x800   /* bit11: VBE 视频信息有效 */

/* 内存映射条目的 type 值 */
#define MULTIBOOT_MMAP_AVAILABLE   1    /* 可用内存，操作系统可以使用 */
#define MULTIBOOT_MMAP_RESERVED    2    /* 保留内存，不可使用 */
#define MULTIBOOT_MMAP_ACPI        3    /* ACPI 可恢复内存 */
#define MULTIBOOT_MMAP_NVS         4    /* ACPI NVS 内存（需要保存） */
#define MULTIBOOT_MMAP_BADRAM      5    /* 坏内存区域，应避免使用 */

/* ---- Multiboot 信息结构 ---- */

/**
 * multiboot_elf_sect_t - ELF 符号表信息
 */
typedef struct {
    uint32_t num;      /* 节区头表条目数 */
    uint32_t size;     /* 每个节区头的大小 */
    uint32_t addr;     /* 节区头表地址 */
    uint32_t shndx;    /* 字符串表的节区索引 */
} __attribute__((packed)) multiboot_elf_sect_t;

/**
 * multiboot_aout_sym_t - a.out 符号表信息
 */
typedef struct {
    uint32_t tabsize;   /* 符号表大小 */
    uint32_t strsize;   /* 字符串表大小 */
    uint32_t addr;      /* 符号表地址 */
    uint32_t reserved;
} __attribute__((packed)) multiboot_aout_sym_t;

/**
 * multiboot_module_t - 引导模块信息
 * 引导器可以在内存中加载辅助模块（如 ramdisk/初始文件系统），
 * mod_start/mod_end 标识其在物理内存中的位置。
 */
typedef struct {
    uint32_t mod_start;     /* 模块起始物理地址 */
    uint32_t mod_end;       /* 模块结束物理地址 */
    uint32_t string;        /* 模块名/参数字符串地址 */
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

/**
 * multiboot_mmap_entry_t - 内存映射条目
 *
 * 每个条目描述一段连续的物理地址空间及其类型。
 * size 字段（4 字节）表示此条目的字节数（不包括 size 自身），
 * 遍历时应跳过 size + 4 字节。每个条目至少 20 字节。
 */
typedef struct {
    uint32_t size;              /* 条目大小（不包含自身） */
    uint64_t base_addr_low;     /* 基地址低 32 位 */
    uint64_t base_addr_high;    /* 基地址高 32 位（物理地址 > 4GB 使用） */
    uint64_t length_low;        /* 长度低 32 位 */
    uint64_t length_high;       /* 长度高 32 位 */
    uint32_t type;              /* 1=可用, 2=保留, 3=ACPI, 4=NVS, 5=坏内存 */
} __attribute__((packed)) multiboot_mmap_entry_t;

/**
 * multiboot_info_t - 主信息结构体（由 GRUB 填充）
 *
 * GRUB 在跳转到内核之前会将此结构体放在内存中，并将地址存入 ebx。
 * 内核通过检查 flags 字段判断哪些字段有效。
 * 注意：所有地址字段都是物理地址，在启用分页前需要直接使用。
 */
typedef struct {
    /* 基本信息（通常都可用） */
    uint32_t flags;             /* 位掩码，标明哪些字段有效 */
    uint32_t mem_lower;         /* 低位内存大小（KB），通常为 640KB */
    uint32_t mem_upper;         /* 高位内存大小（KB），从 1MB 往上 */
    uint32_t boot_device;       /* 引导设备（BIOS 设备号编码） */
    uint32_t cmdline;           /* 内核命令行字符串的物理地址 */
    uint32_t mods_count;        /* 引导模块数量 */
    uint32_t mods_addr;         /* 模块数组的物理地址 */

    /* 符号表（二者之一有效，由 flags 判断） */
    union {
        multiboot_aout_sym_t aout_sym;
        multiboot_elf_sect_t elf_sec;
    } syms;

    /* 内存映射（bit6 置位时有效） */
    uint32_t mmap_length;       /* MMAP 数组的总字节数 */
    uint32_t mmap_addr;         /* MMAP 数组的物理地址 */

    /* 以下为可选信息 */
    uint32_t drives_length;     /* 驱动器信息数组长度 */
    uint32_t drives_addr;       /* 驱动器信息数组地址 */
    uint32_t config_table;      /* ROM 配置表地址 */
    uint32_t boot_loader_name;  /* 引导加载器名称字符串地址 */

    /* APM（高级电源管理）表 */
    uint32_t apm_table;

    /* VBE（VESA BIOS 扩展）视频信息 */
    uint32_t vbe_control_info;  /* VBE 控制信息地址 */
    uint32_t vbe_mode_info;     /* VBE 模式信息地址 */
    uint32_t vbe_mode;          /* VBE 当前模式 */
    uint32_t vbe_interface_seg; /* VBE 接口段地址 */
    uint32_t vbe_interface_off; /* VBE 接口偏移 */
    uint32_t vbe_interface_len; /* VBE 接口代码长度 */
} __attribute__((packed)) multiboot_info_t;

#endif
