/*
 * vfs.c - 虚拟文件系统：挂载表 + 路径路由
 * 路径前缀 → 挂载点 → FAT 实例；shell 可挂载/卸载 USB 盘。
 * 内核单线程：FAT 实例内部自带 irq_lock，这里无需额外加锁。
 */

#include "../include/driver/vga.h"
#include "../include/lib/string.h"
#include "../include/fs/vfs.h"
#include <stdint.h>
#include <stdbool.h>

static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static const block_dev_t* devs[VFS_MAX_DEVS];
static uint32_t dev_count = 0;

/* 挂载点字符串必须拷贝进内核自有缓冲：syscall 传入的用户指针
 * 指向 shell 栈帧，命令返回后即失效，直接存表里会悬空。 */
static char mount_point_storage[VFS_MAX_MOUNTS][32];

/* 注册块设备：查重名（strcmp 相同返回 false）→ 占槽位（满返回 false） */
bool vfs_register_device(const block_dev_t* dev) {
    if (!dev || !dev->name) return false;
    for (uint32_t i = 0; i < dev_count; i++) {
        if (strcmp(devs[i]->name, dev->name) == 0) return false;   /* 重名 */
    }
    if (dev_count >= VFS_MAX_DEVS) return false;
    devs[dev_count++] = dev;
    return true;
}

/* 挂载：dev_name 匹配已注册设备 → fat_mount → 占槽位 */
bool vfs_mount(const char* point, const char* dev_name) {
    if (!point || !dev_name) return false;

    /* 按名字找已注册设备 */
    const block_dev_t* dev = NULL;
    for (uint32_t i = 0; i < dev_count; i++) {
        if (strcmp(devs[i]->name, dev_name) == 0) { dev = devs[i]; break; }
    }
    if (!dev) {
        vga_write("[VFS] Mount failed: unknown device '");
        vga_write(dev_name);
        vga_write("'\n");
        return false;
    }

    /* 找空槽；mount_point 相同则重挂 */
    vfs_mount_t* slot = NULL;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].mounted && mounts[i].mount_point &&
            strcmp(mounts[i].mount_point, point) == 0) {
            slot = &mounts[i];
            break;
        }
    }
    if (!slot) {
        for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
            if (!mounts[i].mounted) { slot = &mounts[i]; break; }
        }
    }
    if (!slot) {
        vga_write("[VFS] Mount table full.\n");
        return false;
    }

    /* 挂载点拷贝进内核缓冲（见 mount_point_storage 注释） */
    uint32_t idx = (uint32_t)(slot - mounts);
    strncpy(mount_point_storage[idx], point, sizeof(mount_point_storage[idx]) - 1);
    mount_point_storage[idx][sizeof(mount_point_storage[idx]) - 1] = '\0';

    if (!fat_mount(&slot->fs, dev)) {
        vga_write("[VFS] Mount failed: no FAT filesystem on '");
        vga_write(dev_name);
        vga_write("'\n");
        return false;
    }
    slot->mount_point = mount_point_storage[idx];
    slot->dev = dev;
    slot->mounted = true;
    return true;
}

/* 卸载：按挂载点找槽，仅标记未挂载（fat 无卸载逻辑，无需释放） */
bool vfs_umount(const char* point) {
    if (!point) return false;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].mounted && mounts[i].mount_point &&
            strcmp(mounts[i].mount_point, point) == 0) {
            mounts[i].mounted = false;
            return true;
        }
    }
    return false;
}

/* 路径解析（关键语义）：
 * - path==NULL 或空串 → 返回根挂载（"/"）的 &fs，subpath=NULL
 * - 否则在所有 mounted 槽里做最长前缀匹配：
 *   strncmp(mount_point, path, len)==0 且 path[len]=='\0' 或 '/'
 * - 有匹配：subpath = path[len]=='\0' ? NULL : path+len+1（剥完为空 → NULL）
 * - 无匹配：回退根挂载，subpath = path[0]=='/' ? path+1 : path
 * - 根挂载不存在 → 返回 NULL */
fat_fs_t* vfs_resolve(const char* path, const char** subpath) {
    if (subpath) *subpath = NULL;

    /* 空路径 → 根挂载 */
    if (!path || path[0] == '\0') {
        for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
            if (mounts[i].mounted && mounts[i].mount_point &&
                strcmp(mounts[i].mount_point, "/") == 0) {
                return &mounts[i].fs;
            }
        }
        return NULL;
    }

    /* 最长前缀匹配 */
    vfs_mount_t* best = NULL;
    uint32_t best_len = 0;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].mounted || !mounts[i].mount_point) continue;
        uint32_t len = strlen(mounts[i].mount_point);
        if (len > best_len &&
            strncmp(mounts[i].mount_point, path, len) == 0 &&
            (path[len] == '\0' || path[len] == '/')) {
            best = &mounts[i];
            best_len = len;
        }
    }
    if (best) {
        if (subpath) {
            if (path[best_len] == '\0') {
                *subpath = NULL;                     /* 挂载点本身 */
            } else {
                const char* sub = path + best_len + 1;  /* 跳过 '/' */
                *subpath = (sub[0] == '\0') ? NULL : sub;
            }
        }
        return &best->fs;
    }

    /* 无匹配：回退根挂载 */
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].mounted && mounts[i].mount_point &&
            strcmp(mounts[i].mount_point, "/") == 0) {
            if (subpath) *subpath = (path[0] == '/') ? path + 1 : path;
            return &mounts[i].fs;
        }
    }
    return NULL;
}

/* FAT 类型 → 名字（0=unknown） */
static const char* fat_type_name(fat_type_t t) {
    switch (t) {
        case FAT12: return "FAT12";
        case FAT16: return "FAT16";
        case FAT32: return "FAT32";
        default:    return "unknown";
    }
}

/* 虚拟 /dev 目录内容：枚举已注册块设备，生成伪目录项。
 * Linux 里 /dev 是 devfs（设备节点由内核动态生成）；这里简化：
 * ls /dev 直接列出设备名（ata0/usb0...），大小=容量。 */
uint32_t vfs_list_devices(fat_dirent_t* entries, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < dev_count && n < max; i++) {
        const char* name = devs[i]->name;
        /* 去 "/dev/" 前缀（6 字符），拿设备名 */
        if (name[0] == '/' && name[1] == 'd' && name[2] == 'e' &&
            name[3] == 'v' && name[4] == '/') {
            name += 5;
        }
        fat_dirent_t* e = &entries[n];
        /* 清空 32 字节（避免垃圾） */
        for (int j = 0; j < 32; j++) ((uint8_t*)e)[j] = 0;
        /* 8.3 名：大写 + 补空格 */
        unsigned int k = 0;
        while (name[k] && k < 11) {
            char c = name[k];
            if (c >= 'a' && c <= 'z') c -= 32;
            e->name[k++] = (uint8_t)c;
        }
        while (k < 11) e->name[k++] = ' ';
        e->attributes = 0x20;                     /* 普通文件（简化） */
        e->file_size = devs[i]->sector_count * 512;
        n++;
    }
    return n;
}

/* 把挂载点路径的最后一段提取为 8.3 目录名（"/mnt/usb" → "USB"）。
 * 返回名字长度（0=根挂载点，无名字）。 */
static unsigned int mount_point_name(const char* point, char* out, unsigned int out_size) {
    /* 找最后一个 '/' 之后的部分 */
    const char* last = NULL;
    for (const char* q = point; *q; q++) {
        if (*q == '/') last = q + 1;
    }
    if (!last || *last == '\0') return 0;
    unsigned int k = 0;
    while (last[k] && k < out_size - 1) {
        char c = last[k];
        if (c >= 'a' && c <= 'z') c -= 32;   /* 8.3 大写 */
        out[k] = c;
        k++;
    }
    out[k] = '\0';
    return k;
}

/* 虚拟 /mnt 目录内容：列出所有已挂载的非根挂载点（挂载点最后一段），
 * 属性=目录。挂载了就出现、卸载就消失——挂载点由系统挂载出来。 */
uint32_t vfs_list_mount_points(fat_dirent_t* entries, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS && n < max; i++) {
        if (!mounts[i].mounted || !mounts[i].mount_point) continue;
        if (strcmp(mounts[i].mount_point, "/") == 0) continue;  /* 跳过根挂载 */
        char name[12];
        unsigned int len = mount_point_name(mounts[i].mount_point, name, sizeof(name));
        if (len == 0) continue;
        fat_dirent_t* e = &entries[n];
        for (int j = 0; j < 32; j++) ((uint8_t*)e)[j] = 0;
        unsigned int k = 0;
        while (k < len && k < 11) { e->name[k] = (uint8_t)name[k]; k++; }
        while (k < 11) e->name[k++] = ' ';
        e->attributes = 0x10;                    /* 目录 */
        n++;
    }
    return n;
}

/* 打印设备与挂载表（devices 命令用，内核态 vga 输出） */
void vfs_print_devices(void) {
    vga_write("[VFS] Registered devices:\n");
    for (uint32_t i = 0; i < dev_count; i++) {
        vga_write("  ");
        vga_write(devs[i]->name);
        vga_write("  sectors=");
        vga_write_hex(devs[i]->sector_count);
        vga_write("\n");
    }
    vga_write("[VFS] Mount table:\n");
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        vga_write("  ");
        vga_write(mounts[i].mounted ? "[M] " : "[ ] ");
        if (mounts[i].mount_point) {
            vga_write(mounts[i].mount_point);
            if (mounts[i].dev) {
                vga_write(" -> ");
                vga_write(mounts[i].dev->name);
            }
            vga_write(" [");
            vga_write(fat_type_name(fat_get_type(&mounts[i].fs)));
            vga_write("]");
        } else {
            vga_write("(empty)");
        }
        vga_write("\n");
    }
}
