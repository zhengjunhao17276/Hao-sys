/*
 * multiboot.h - Multiboot 信息结构（GRUB 经 ebx 传入，物理地址）
 * 内核先查 info->flags 各位确定哪些字段有效。
 */

#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* ---- 信息标志位 ---- */
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
#define MULTIBOOT_MMAP_AVAILABLE   1    /* 可用内存 */
#define MULTIBOOT_MMAP_RESERVED    2    /* 保留 */
#define MULTIBOOT_MMAP_ACPI        3    /* ACPI 可恢复内存 */
#define MULTIBOOT_MMAP_NVS         4    /* ACPI NVS 内存（需要保存） */
#define MULTIBOOT_MMAP_BADRAM      5    /* 坏内存区域，应避免使用 */

/* ---- Multiboot 信息结构 ---- */

/* ELF 符号表信息 */
typedef struct {
    uint32_t num;      /* 节区头表条目数 */
    uint32_t size;     /* 每个节区头的大小 */
    uint32_t addr;     /* 节区头表地址 */
    uint32_t shndx;    /* 字符串表的节区索引 */
} __attribute__((packed)) multiboot_elf_sect_t;

/* a.out 符号表信息 */
typedef struct {
    uint32_t tabsize;   /* 符号表大小 */
    uint32_t strsize;   /* 字符串表大小 */
    uint32_t addr;      /* 符号表地址 */
    uint32_t reserved;
} __attribute__((packed)) multiboot_aout_sym_t;

/* 引导模块（ramdisk 等），mod_start/mod_end 为物理地址 */
typedef struct {
    uint32_t mod_start;     /* 模块起始物理地址 */
    uint32_t mod_end;       /* 模块结束物理地址 */
    uint32_t string;        /* 模块名/参数字符串地址 */
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

/* MMAP 条目：遍历时跳过 size+4 字节，每条目至少 20 字节 */
typedef struct {
    uint32_t size;              /* 条目大小（不包含自身） */
    uint64_t base_addr_low;     /* 基地址低 32 位 */
    uint64_t base_addr_high;    /* 基地址高 32 位（物理地址 > 4GB 使用） */
    uint64_t length_low;        /* 长度低 32 位 */
    uint64_t length_high;       /* 长度高 32 位 */
    uint32_t type;              /* 1=可用, 2=保留, 3=ACPI, 4=NVS, 5=坏内存 */
} __attribute__((packed)) multiboot_mmap_entry_t;

/* 主信息结构（GRUB 填充）。地址字段都是物理地址，分页前直接使用 */
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
