/*
 * probe.h - 文件系统识别（probe）
 * 读块设备关键扇区/签名，识别文件系统类型，供挂载前检测与 devices 展示。
 * 目前仅 FAT12/16/32 可挂载；其余识别出来用于报错提示（"Unsupported filesystem"）。
 */

#ifndef PROBE_H
#define PROBE_H

#include "../driver/block_dev.h"

/* 识别结果 */
typedef enum {
    FS_UNKNOWN = 0,
    FS_FAT12, FS_FAT16, FS_FAT32,     /* 微软 FAT 家族 */
    FS_EXFAT,                          /* exFAT */
    FS_NTFS,                           /* NTFS */
    FS_EXT2, FS_EXT3, FS_EXT4,         /* Linux ext 家族 */
    FS_ISO9660,                        /* 光盘 */
    FS_XFS,
    FS_BTRFS
} probe_fs_t;

/* 类型名（vga/提示用），未知返回 "unknown" */
const char* probe_fs_name(probe_fs_t type);

/* 探测块设备上的文件系统类型（读扇区 0/2/16/128，失败返回 FS_UNKNOWN） */
probe_fs_t fs_probe(const block_dev_t* dev);

#endif
