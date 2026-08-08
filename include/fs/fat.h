/*
 * fat.h - FAT 文件系统驱动接口
 * FAT12/16/32：12/16/32 位簇号（FAT32 实际只用低 28 位）。
 * 磁盘布局：保留区 → FAT×N → 根目录区 → 数据区（簇 #2 起）。
 * 仅支持 8.3 短文件名，支持读写与子目录。
 */

#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stdbool.h>
#include "../driver/block_dev.h"

typedef enum {
    FAT_UNKNOWN = 0,  /* 未识别 */
    FAT12,            /* 12 位簇号 */
    FAT16,            /* 16 位簇号 */
    FAT32             /* 32 位簇号 */
} fat_type_t;

/*
 * fat_bpb_t - 磁盘第 0 扇区的 BPB（FAT12/16 前 36 字节，FAT32 前 90 字节）。
 * 必须 packed：直接从扇区缓冲读取，不允许有结构体填充字节。
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

/*
 * fat_dirent_t - 目录项（32 字节）
 * 名字 11 字节 = 8 文件名 + 3 扩展名，不足右补空格；
 * 首字节 0x00=目录结束，0xE5=已删除；
 * first_cluster = (cluster_high << 16) | cluster_low。
 */
typedef struct __attribute__((packed)) {
    uint8_t  name[11];              /* 8.3 格式文件名（短文件名） */
    uint8_t  attributes;            /* 文件属性（只读/隐藏/系统/卷标/目录/归档） */
    uint8_t  nt_reserved;           /* NT 保留（用于大小写信息） */
    uint8_t  creation_time_tenths;  /* ⚠️ 借用：Unix mode 低字节（时间未实现恒 0） */
    uint16_t creation_time;         /* ⚠️ 借用：低字节=Unix mode 高字节，高字节=魔数 */
    uint16_t creation_date;         /* ⚠️ 借用：Unix uid（小端） */
    uint16_t last_access_date;      /* ⚠️ 借用：Unix gid（小端） */
    uint16_t cluster_high;          /* FAT32 簇号高 16 位 */
    uint16_t last_write_time;       /* 最后修改时间 */
    uint16_t last_write_date;       /* 最后修改日期 */
    uint16_t cluster_low;           /* 簇号低 16 位 */
    uint32_t file_size;             /* 文件大小（字节），目录此项为 0 */
} fat_dirent_t;

/* ---- Linux 权限扩展（存于 FAT 目录项的借用字段） ----
 * 本内核未实现目录项时间戳（所有时间字段恒 0），借用：
 *   offset 13 (creation_time_tenths) = mode 低字节
 *   offset 14 (creation_time 低字节)  = mode 高字节
 *   offset 15 (creation_time 高字节)  = 魔数 0x58（无魔数 → 默认 meta）
 *   offset 16-17 (creation_date)      = uid（小端）
 *   offset 18-19 (last_access_date)   = gid（小端）
 * 默认 meta：目录 0755、文件 0644，uid=gid=0。
 * ⚠️ 将来实现目录项时间戳时需迁移（换字段或独立 xattr）。 */
#define FAT_META_MAGIC 0x58

/* Linux 风格权限位（与 POSIX 数值一致） */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

/* 解析目录项的 Unix meta（无魔数 → 默认值） */
void fat_entry_unix_meta(const fat_dirent_t* e, uint16_t* mode, uint16_t* uid, uint16_t* gid);
/* 把 Unix meta 写入目录项（含魔数） */
void fat_entry_set_unix_meta(fat_dirent_t* e, uint16_t mode, uint16_t uid, uint16_t gid);

/* fat_fs_t - FAT 实例：一个挂载的文件系统对应一份状态。
 * sector_buffer/dir_scan_entries 不能共享（并发访问会互相踩踏）。 */
typedef struct {
    fat_bpb_t bpb;
    const block_dev_t* dev;        /* 底层块设备 */
    uint32_t base_lba;             /* 逻辑卷起始 LBA（直格=0，MBR 分区=分区起始） */
    uint32_t first_data_sector;    /* 数据区起始 LBA */
    uint32_t root_dir_sectors;     /* FAT12/16 根目录占用扇区数 */
    uint32_t total_sectors;
    uint32_t fat_size;             /* 每份 FAT 扇区数 */
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_sector;
    uint32_t root_dir_entries;
    fat_type_t fs_type;
    bool initialized;
    uint32_t root_cluster;         /* FAT32 根目录起始簇 */
    uint8_t sector_buffer[512];    /* 单扇区缓冲（每实例独立） */
    fat_dirent_t dir_scan_entries[128];  /* 目录扫描缓冲（每实例独立） */
} fat_fs_t;

/* 挂载块设备：读 BPB 并识别 FAT 类型，成功返回 true */
bool fat_mount(fat_fs_t* fs, const block_dev_t* dev);

/* 返回检测到的 FAT 类型 */
fat_type_t fat_get_type(fat_fs_t* fs);

/* 读根目录条目（FAT32 跟随根簇链，FAT12/16 读固定区），返回条目数 */
uint32_t fat_read_root_dir(fat_fs_t* fs, fat_dirent_t* entries, uint32_t max_entries);

/* 按 8.3 名查找文件（大小写不敏感），找到填 out_entry 并返回 true */
bool fat_find_file(fat_fs_t* fs, const char* filename, fat_dirent_t* out_entry);

/* 沿簇链读文件到 buffer，最多 max_size 字节；返回实际读到的字节数 */
uint32_t fat_load_file(fat_fs_t* fs, const fat_dirent_t* entry, void* buffer, uint32_t max_size);

/* 从文件偏移 offset 处读最多 max_size 字节（分块读大文件用） */
uint32_t fat_load_file_off(fat_fs_t* fs, const fat_dirent_t* entry,
                           uint32_t offset, void* buffer, uint32_t max_size);

/* 新建/覆盖文件，返回 true=成功 */
bool fat_write_file(fat_fs_t* fs, const char* filename, const void* data, uint32_t size);

/* 删除文件或空目录，返回 true=成功 */
bool fat_delete_file(fat_fs_t* fs, const char* filename);

/* 创建子目录（支持多级路径），返回 true=成功 */
bool fat_mkdir(fat_fs_t* fs, const char* path);

/* 读任意目录（0=根目录），返回条目数 */
uint32_t fat_read_dir(fat_fs_t* fs, uint32_t dir_cluster, fat_dirent_t* entries, uint32_t max_entries);

/* 解析目录路径，返回目录首簇号（0=根）；失败返回 0xFFFFFFFF */
uint32_t fat_open_dir(fat_fs_t* fs, const char* path);

/* 按路径读 meta（stat 用），失败返回 false */
bool fat_get_file_meta(fat_fs_t* fs, const char* filename, uint16_t* mode, uint16_t* uid, uint16_t* gid);
/* 按路径写 meta（chmod/chown 用），失败返回 false */
bool fat_set_file_meta(fat_fs_t* fs, const char* filename, uint16_t mode, uint16_t uid, uint16_t gid);

#endif
