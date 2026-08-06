/*
 * vfs.h - 虚拟文件系统：挂载表 + 路径路由
 * 路径前缀路由到不同 FAT 实例，shell 可挂载/卸载 USB 盘（Unix 式挂载）。
 */

#ifndef VFS_H
#define VFS_H

#include "../driver/block_dev.h"
#include "fat.h"

#define VFS_MAX_MOUNTS 8
#define VFS_MAX_DEVS   8

/* 挂载表项：一个挂载点对应一个内嵌 FAT 实例 */
typedef struct {
    const char* mount_point;   /* "/"、"/usb" 等 */
    const block_dev_t* dev;
    fat_fs_t fs;               /* 内嵌 FAT 实例 */
    bool mounted;
} vfs_mount_t;

/* 注册块设备（ata0 启动时、usb0 MSC 探测到时） */
bool vfs_register_device(const block_dev_t* dev);
/* 挂载：dev_name 匹配已注册设备 → fat_mount → 占槽位 */
bool vfs_mount(const char* point, const char* dev_name);
bool vfs_umount(const char* point);
/* 路径解析：最长前缀匹配挂载点；subpath 输出剥离前缀后的相对路径
 * （根目录或空 → subpath=NULL）；无匹配/失败返回 NULL */
fat_fs_t* vfs_resolve(const char* path, const char** subpath);
/* 打印设备与挂载表（devices 命令用，内核态 vga 输出） */
void vfs_print_devices(void);

#endif
