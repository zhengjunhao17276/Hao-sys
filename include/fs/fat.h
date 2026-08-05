/**
 * =========================================================================
 * fat.h - FAT 文件系统驱动接口
 *
 * FAT（File Allocation Table）是 DOS/Windows 传统的文件系统，结构简单
 * 且文档完善，非常适合作为操作系统的第一个文件系统驱动实现。
 *
 * FAT 的三种变体：
 *   FAT12 - 小容量（软盘等，< 32MB），12 位簇号
 *   FAT16 - 中等容量（32MB ~ 2GB），16 位簇号
 *   FAT32 - 大容量（~ 2TB），32 位簇号（但实际只用低 28 位）
 *
 * 磁盘布局（从上电读出的第 0 扇区开始）：
 *   [保留区] → [FAT 表 × N] → [根目录区] → [数据区]
 *   其中 BPB（BIOS Parameter Block）位于第 0 扇区的开头，描述了
 *   整个文件系统的几何参数。
 *
 * 功能范围：
 *   - 读 + 写支持（fat_write_file 新建文件）
 *   - 长文件名未支持（仅支持 8.3 短文件名格式）
 *   - 子目录未支持（当前仅读取根目录）
 * =========================================================================
 */

#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stdbool.h>

/* ---- FAT 类型枚举 ---- */
typedef enum {
    FAT_UNKNOWN = 0,  /* 未识别 */
    FAT12,            /* 12 位簇号 */
    FAT16,            /* 16 位簇号 */
    FAT32             /* 32 位簇号 */
} fat_type_t;

/**
 * fat_bpb_t - FAT BIOS Parameter Block
 * 磁盘第 0 扇区的前 36（FAT12/16）或 90（FAT32）字节。
 * 所有字段都使用 __attribute__((packed)) 确保按字节紧密排列，
 * 因为直接从扇区缓冲区读取时不允许有结构体填充字节。
 *
 * 关键字段说明：
 *   - bytes_per_sector: 一般为 512，逻辑扇区大小
 *   - sectors_per_cluster: 每簇扇区数（FAT 的最小分配单元）
 *   - reserved_sectors: 保留扇区数（FAT12/16 通常为 1，FAT32 通常为 32）
 *   - num_fats: FAT 表的副本数（通常为 2，一份备份）
 *   - root_entry_count: 根目录最大条目数（FAT12/16 用，FAT32 根目录在数据区）
 *   - fat_size_16 / fat_size_32: 每个 FAT 表的扇区数
 *   - root_cluster: FAT32 中根目录的起始簇号
 */
typedef struct __attribute__((packed)) {
    uint8_t  jump_boot[3];      /* 跳转指令（跳转到引导代码），x86 实模式 */
    char     oem_name[8];       /* OEM 名称标识，如 "MSWIN4.1" */
    uint16_t bytes_per_sector;  /* 每扇区字节数，固定 512 */
    uint8_t  sectors_per_cluster;   /* 每簇扇区数（1, 2, 4, 8, 16, 32, 64…） */
    uint16_t reserved_sectors;  /* 保留扇区数（FAT 表之前的区域） */
    uint8_t  num_fats;          /* FAT 表数量，通常为 2 */
    uint16_t root_entry_count;  /* 根目录最大目录项数（FAT12/16 特有） */
    uint16_t total_sectors_16;  /* 总扇区数（如果 < 65536 用此字段，否则用 total_sectors_32） */
    uint8_t  media_descriptor;  /* 介质描述符（0xF0=软盘, 0xF8=硬盘） */
    uint16_t fat_size_16;       /* FAT12/16: 每 FAT 表扇区数（FAT32 用 fat_size_32） */
    uint16_t sectors_per_track; /* 每磁道扇区数（C/H/S 寻址，LBA 时代已废弃） */
    uint16_t num_heads;         /* 磁头数（C/H/S） */
    uint32_t hidden_sectors;    /* 隐藏扇区数（分区偏移） */
    uint32_t total_sectors_32;  /* 总扇区数（如果 total_sectors_16=0 用此字段） */
    /* --- FAT32 扩展字段（FAT12/16 到此为止） --- */
    uint32_t fat_size_32;       /* FAT32: 每 FAT 表扇区数 */
    uint16_t ext_flags;         /* 扩展标志 */
    uint16_t fs_version;        /* 文件系统版本号 */
    uint32_t root_cluster;      /* FAT32: 根目录的起始簇号 */
    uint16_t fs_info;           /* FAT32: FSINFO 扇区号 */
    uint16_t backup_boot_sector;    /* 引导扇区备份扇区号 */
    uint8_t  reserved[12];      /* 保留 */
    uint8_t  drive_number;      /* BIOS 驱动器号（0x80=第一硬盘） */
    uint8_t  reserved1;         /* 保留 */
    uint8_t  boot_signature;    /* 扩展引导签名（0x29 表示后面三个字段有效） */
    uint32_t volume_id;         /* 卷序列号 */
    char     volume_label[11];  /* 卷标（不足 11 字符右补空格） */
    char     fs_type[8];        /* 文件系统类型字符串，如 "FAT32   " */
} fat_bpb_t;

/**
 * fat_dirent_t - FAT 目录项结构（32 字节）
 * 每个目录项代表一个文件或子目录。
 *
 * 文件名的存储方式：
 *   11 字节空间 = 8 字节文件名 + 3 字节扩展名
 *   - 文件名不足 8 字符右补空格
 *   - 扩展名不足 3 字符右补空格
 *   - 首字节特殊值：0x00=目录结束，0xE5=已删除
 *
 * 簇号由 cluster_low 和 cluster_high 拼接得出：
 *   first_cluster = (cluster_high << 16) | cluster_low
 */
typedef struct __attribute__((packed)) {
    uint8_t  name[11];              /* 8.3 格式文件名（短文件名） */
    uint8_t  attributes;            /* 文件属性（只读/隐藏/系统/卷标/目录/归档） */
    uint8_t  nt_reserved;           /* NT 保留（用于大小写信息） */
    uint8_t  creation_time_tenths;  /* 创建时间（毫秒级） */
    uint16_t creation_time;         /* 创建时间 */
    uint16_t creation_date;         /* 创建日期 */
    uint16_t last_access_date;      /* 最后访问日期 */
    uint16_t cluster_high;          /* FAT32 簇号高 16 位 */
    uint16_t last_write_time;       /* 最后修改时间 */
    uint16_t last_write_date;       /* 最后修改日期 */
    uint16_t cluster_low;           /* 簇号低 16 位 */
    uint32_t file_size;             /* 文件大小（字节），目录此项为 0 */
} fat_dirent_t;

/* ---- 函数声明 ---- */

/**
 * fat_init - 初始化 FAT 文件系统
 * 读取第 0 扇区的 BPB，检测 FAT 类型（FAT12/16/32），
 * 计算数据区起始偏移和根目录位置。
 * 返回 true 表示成功识别 FAT 文件系统。
 */
bool fat_init(void);

/**
 * fat_get_type - 获取 FAT 类型
 * 返回 fat_init 检测到的 FAT 类型枚举。
 */
fat_type_t fat_get_type(void);

/**
 * fat_read_root_dir - 读取根目录所有条目
 * @entries:     用于存放目录项的数组
 * @max_entries: 数组最大容量
 * 返回值：实际读取到的目录项数量
 *
 * 对 FAT32 会跟随根目录簇链读取，对 FAT12/16 读取固定大小的根目录区。
 */
uint32_t fat_read_root_dir(fat_dirent_t* entries, uint32_t max_entries);

/**
 * fat_find_file - 在根目录中查找指定文件
 * @filename:  文件名（如 "SHELL.BIN"，大小写不敏感）
 * @out_entry: 输出参数，找到的目录项
 * 返回 true 表示找到文件，false 表示不存在。
 *
 * 内部将文件名转换为 8.3 格式（大写并右补空格）后与目录项匹配。
 */
bool fat_find_file(const char* filename, fat_dirent_t* out_entry);

/**
 * fat_load_file - 读取文件内容到缓冲区
 * @entry:    要读取的目录项
 * @buffer:   目标缓冲区
 * @max_size: 缓冲区最大容量
 * 返回值：实际读取的字节数，失败返回 0
 *
 * 遍历 FAT 簇链，逐个读取数据簇到缓冲区。
 * 如果文件大小超过 max_size，只读取 max_size 字节。
 */
uint32_t fat_load_file(const fat_dirent_t* entry, void* buffer, uint32_t max_size);

/**
 * fat_write_file - 在根目录新建一个文件并写入数据
 * @filename: 文件名（如 "HELLO.TXT"，大小写不敏感）
 * @data:     数据源指针（内核地址空间可见即可）
 * @size:     数据字节数
 * 返回 true=成功，false=失败（文件已存在 / 空间不足 / 根目录满）
 *
 * 自动分配簇链、写 FAT（含 FAT2 镜像）、填根目录项。
 * v1 不支持覆盖已有文件，不支持子目录。
 */
bool fat_write_file(const char* filename, const void* data, uint32_t size);

/**
 * fat_delete_file - 删除文件或空目录（支持 "DIR/FILE" 路径）
 * 返回 true=成功，false=失败（不存在 / 非空目录）。
 */
bool fat_delete_file(const char* filename);

/**
 * fat_mkdir - 创建子目录（支持多级路径）
 * 返回 true=成功，false=失败（已存在 / 路径不存在 / 空间不足）。
 */
bool fat_mkdir(const char* path);

/**
 * fat_read_dir - 读取任意目录（0=根目录），返回条目数
 */
uint32_t fat_read_dir(uint32_t dir_cluster, fat_dirent_t* entries, uint32_t max_entries);

/**
 * fat_open_dir - 解析目录路径，返回目录首簇号（0=根目录）
 * 失败（不存在或不是目录）返回 0xFFFFFFFF。
 */
uint32_t fat_open_dir(const char* path);

#endif
