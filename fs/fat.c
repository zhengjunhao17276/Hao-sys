/*
 * fat.c - FAT12/16/32 文件系统驱动
 * BPB 解析、类型检测、簇链遍历、目录读写、8.3 文件名查找、
 * 文件加载/写入、目录创建/删除。
 * 所有状态都在 fat_fs_t 实例内，通过函数指针访问底层块设备，
 * 同一份驱动可挂载多块设备（ATA 主盘 / USB MSC）。
 */

#include "../include/driver/vga.h"
#include "../include/lib/string.h"
#include "../include/fs/fat.h"
#include <stdint.h>
#include <stdbool.h>
#include "../include/driver/irqlock.h"

/* 扇区读写统一加 base_lba 偏移（直格盘 base_lba=0 不受影响） */
static bool fat_dev_read(fat_fs_t* fs, uint32_t lba, void* buf) {
    return fs->dev->read_sector(fs->base_lba + lba, buf);
}
static bool fat_dev_write(fat_fs_t* fs, uint32_t lba, const void* buf) {
    return fs->dev->write_sector(fs->base_lba + lba, buf);
}

/* FAT12 项 1.5 字节（奇偶簇取高/低 12 位）；FAT16 直接偏移；
 * FAT32 4 字节仅低 28 位有效 */
static uint32_t read_fat_entry(fat_fs_t* fs, uint32_t cluster) {
    if (fs->fs_type == FAT12) {
        uint32_t fat_offset = cluster * 3 / 2;
        uint32_t byte_off   = fat_offset % fs->bytes_per_sector;
        uint32_t fat_sector = fs->bpb.reserved_sectors + fat_offset / fs->bytes_per_sector;
        /* ⚠️ FAT12 表项可能跨扇区边界（奇簇时低字节落在 511、高字节在
         * 下一扇区 0），旧实现 *(uint16_t*)(sector_buffer+511) 越界读。 */
        uint8_t b0, b1;
        if (byte_off + 1 < fs->bytes_per_sector) {
            if (!fat_dev_read(fs, fat_sector, fs->sector_buffer)) return 0x0FFFFFFF;
            b0 = fs->sector_buffer[byte_off];
            b1 = fs->sector_buffer[byte_off + 1];
        } else {
            if (!fat_dev_read(fs, fat_sector, fs->sector_buffer)) return 0x0FFFFFFF;
            b0 = fs->sector_buffer[byte_off];
            if (!fat_dev_read(fs, fat_sector + 1, fs->sector_buffer)) return 0x0FFFFFFF;
            b1 = fs->sector_buffer[0];
        }
        uint16_t val = (uint16_t)(b0 | (b1 << 8));
        if (cluster & 1) val >>= 4;
        else val &= 0x0FFF;
        return val;
    } else if (fs->fs_type == FAT16) {
        uint32_t fat_offset = cluster * 2;
        uint32_t fat_sector = fs->bpb.reserved_sectors + fat_offset / fs->bytes_per_sector;
        uint32_t offset = fat_offset % fs->bytes_per_sector;
        if (!fat_dev_read(fs, fat_sector, fs->sector_buffer)) return 0xFFFF;
        return *(uint16_t*)(fs->sector_buffer + offset);
    } else if (fs->fs_type == FAT32) {
        uint32_t fat_offset = cluster * 4;
        uint32_t fat_sector = fs->bpb.reserved_sectors + fat_offset / fs->bytes_per_sector;
        uint32_t offset = fat_offset % fs->bytes_per_sector;
        if (!fat_dev_read(fs, fat_sector, fs->sector_buffer)) return 0x0FFFFFFF;
        return *(uint32_t*)(fs->sector_buffer + offset) & 0x0FFFFFFF;
    }
    return 0x0FFFFFFF;
}

/* 微软规范阈值：总簇数 <4085 → FAT12，<65525 → FAT16，否则 FAT32 */
static fat_type_t detect_fat_type(fat_fs_t* fs) {
    if (fs->bpb.bytes_per_sector != 512) {
        vga_write("[FAT] Warning: bytes_per_sector != 512, may not be FAT.\n");
        return FAT_UNKNOWN;
    }
    if (fs->bpb.sectors_per_cluster == 0 || fs->bpb.sectors_per_cluster > 64) {
        vga_write("[FAT] Invalid sectors_per_cluster.\n");
        return FAT_UNKNOWN;
    }
    uint32_t root_dir_sectors_calc =
        ((fs->bpb.root_entry_count * 32) + fs->bpb.bytes_per_sector - 1) / fs->bpb.bytes_per_sector;
    uint32_t fat_size_used = fs->bpb.fat_size_16 ? fs->bpb.fat_size_16 : fs->bpb.fat_size_32;
    uint32_t total_sectors_used = fs->bpb.total_sectors_16 ? fs->bpb.total_sectors_16 : fs->bpb.total_sectors_32;
    if (fat_size_used == 0) {
        vga_write("[FAT] FAT size is zero, cannot determine type.\n");
        return FAT_UNKNOWN;
    }
    uint32_t data_sectors =
        total_sectors_used - (fs->bpb.reserved_sectors + fs->bpb.num_fats * fat_size_used + root_dir_sectors_calc);
    uint32_t total_clusters = data_sectors / fs->bpb.sectors_per_cluster;
    if (total_clusters < 4085) return FAT12;
    else if (total_clusters < 65525) return FAT16;
    else return FAT32;
}

/* MBR 分区表：找第一个 FAT 分区（类型 01/04/06/0E/0B/0C），返回起始 LBA；
 * 无 MBR 签名/无 FAT 分区返回 0。分区项在偏移 446，每项 16 字节，
 * 类型字节在 +4，起始 LBA 在 +8（小端 32 位）。 */
static uint32_t mbr_find_fat_partition(const uint8_t* mbr) {
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        if (type == 0x00 || type == 0x05 || type == 0x0F) continue;  /* 空槽/扩展分区 */
        if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0E ||
            type == 0x0B || type == 0x0C) {
            return (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                   ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        }
    }
    return 0;
}

/* 校验 sector_buffer 中的 BPB 并填派生字段（fat_mount 对直格盘和 MBR
 * 分区盘各调一次，sector_buffer 须已含待校验的引导扇区） */
static bool fat_bpb_load(fat_fs_t* fs) {
    fat_bpb_t* b = (fat_bpb_t*)fs->sector_buffer;
    fs->bpb = *b;

    if (fs->sector_buffer[510] != 0x55 || fs->sector_buffer[511] != 0xAA) {
        vga_write("[FAT] Invalid boot sector signature (0x55AA missing).\n");
        return false;
    }

    fs->fs_type = FAT_UNKNOWN;
    if (memcmp(fs->bpb.fs_type, "FAT12", 5) == 0) fs->fs_type = FAT12;
    else if (memcmp(fs->bpb.fs_type, "FAT16", 5) == 0) fs->fs_type = FAT16;
    else if (memcmp(fs->bpb.fs_type, "FAT32", 5) == 0) fs->fs_type = FAT32;

    if (fs->fs_type == FAT_UNKNOWN) {
        fs->fs_type = detect_fat_type(fs);
    }
    if (fs->fs_type == FAT_UNKNOWN) {
        vga_write("[FAT] Unrecognized filesystem.\n");
        return false;
    }

    vga_write("[FAT] ");
    switch (fs->fs_type) {
        case FAT12: vga_write("FAT12"); break;
        case FAT16: vga_write("FAT16"); break;
        case FAT32: vga_write("FAT32"); break;
        default:    vga_write("?"); break;
    }
    vga_write(" filesystem\n");

    fs->bytes_per_sector    = fs->bpb.bytes_per_sector;
    fs->sectors_per_cluster = fs->bpb.sectors_per_cluster;
    fs->root_dir_entries    = fs->bpb.root_entry_count;
    fs->root_dir_sectors    = ((fs->root_dir_entries * 32) + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
    fs->total_sectors       = fs->bpb.total_sectors_16 ? fs->bpb.total_sectors_16 : fs->bpb.total_sectors_32;
    fs->fat_size            = fs->bpb.fat_size_16 ? fs->bpb.fat_size_16 : fs->bpb.fat_size_32;
    fs->first_data_sector   = fs->bpb.reserved_sectors + fs->bpb.num_fats * fs->fat_size + fs->root_dir_sectors;

    if (fs->fs_type == FAT32) {
        fs->root_cluster      = fs->bpb.root_cluster;
        fs->root_dir_sectors  = 0;
        fs->first_data_sector = fs->bpb.reserved_sectors + fs->bpb.num_fats * fs->fat_size;
    }

    fs->initialized = true;
    return true;
}

/* 挂载：先试直格盘（LBA 0 = FAT BPB），失败则解析 MBR 分区表，
 * 从 FAT 分区起始 LBA 挂载（base_lba 偏移）。 */
bool fat_mount(fat_fs_t* fs, const block_dev_t* dev) {
    fs->dev = dev;
    fs->base_lba = 0;

    /* 尝试 1：直格盘（LBA 0 = FAT BPB） */
    if (!fat_dev_read(fs, 0, fs->sector_buffer)) {
        vga_write("[FAT] Failed to read boot sector.\n");
        return false;
    }
    if (fat_bpb_load(fs)) return true;

    /* 尝试 2：MBR 分区盘（LBA 0 = MBR，FAT 在分区起始） */
    uint32_t part_lba = mbr_find_fat_partition(fs->sector_buffer);
    if (part_lba != 0) {
        fs->base_lba = part_lba;
        if (fat_dev_read(fs, 0, fs->sector_buffer) && fat_bpb_load(fs)) return true;
    }

    vga_write("[FAT] No FAT filesystem (raw or MBR) on '");
    vga_write(fs->dev->name);
    vga_write("'\n");
    return false;
}

fat_type_t fat_get_type(fat_fs_t* fs) {
    uint32_t fl = irq_lock();
    fat_type_t t = fs->fs_type;
    irq_unlock(fl);
    return t;
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

/* 转 8.3 格式：大写、右补空格 */
static void make_83_name(const char* filename, char name_83[12]) {
    for (int i = 0; i < 11; i++) name_83[i] = ' ';
    name_83[11] = '\0';
    int i;
    for (i = 0; filename[i] && filename[i] != '.' && i < 8; i++) {
        name_83[i] = to_upper(filename[i]);
    }
    if (filename[i] == '.') {
        i++;
        for (int j = 0; filename[i] && j < 3; i++, j++) {
            name_83[8 + j] = to_upper(filename[i]);
        }
    }
}

/* 读任意目录：0=根目录（FAT12/16 固定区或 FAT32 根簇），跟随簇链。
 * 跳过 0xE5 已删除项，遇 0x00 停止。 */
uint32_t fat_read_dir(fat_fs_t* fs, uint32_t dir_cluster, fat_dirent_t* entries, uint32_t max_entries) {
    uint32_t fl = irq_lock();
    if (!fs->initialized || !entries || max_entries == 0) { irq_unlock(fl); return 0; }

    uint32_t index = 0;
    uint32_t cluster = dir_cluster;
    if (dir_cluster == 0 && fs->fs_type == FAT32) cluster = fs->root_cluster;

    while (1) {
        uint32_t lba, sector_count;
        if (cluster == 0) {
            /* FAT12/16 根目录固定区 */
            lba = fs->bpb.reserved_sectors + fs->bpb.num_fats * fs->fat_size;
            sector_count = fs->root_dir_sectors;
        } else {
            if (cluster < 2) break;
            lba = fs->first_data_sector + (cluster - 2) * fs->sectors_per_cluster;
            sector_count = fs->sectors_per_cluster;
        }

        for (uint32_t s = 0; s < sector_count; s++) {
            if (!fat_dev_read(fs, lba + s, fs->sector_buffer)) break;
            fat_dirent_t* dirent = (fat_dirent_t*)fs->sector_buffer;
            uint32_t per_sector = fs->bytes_per_sector / 32;
            for (uint32_t i = 0; i < per_sector && index < max_entries; i++) {
                if (dirent[i].name[0] == 0x00) { irq_unlock(fl); return index; }
                if ((uint8_t)dirent[i].name[0] == 0xE5) continue;
                entries[index++] = dirent[i];
            }
        }

        if (cluster == 0) break;   /* 根目录固定区读完 */
        uint32_t next = read_fat_entry(fs, cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    irq_unlock(fl);
    return index;
}

/* 根目录条目（兼容包装） */
uint32_t fat_read_root_dir(fat_fs_t* fs, fat_dirent_t* entries, uint32_t max_entries) {
    return fat_read_dir(fs, 0, entries, max_entries);
}

/* ⚠️ 目录扫描缓冲绝不能放栈上：128 项 = 4KB，而每个任务的内核栈只有
 * 4KB，栈数组会写穿到栈页之下（PCB 页），是定时炸弹（实测侥幸未炸）。
 * 内核单线程，全局缓冲安全。 */
static bool dir_find_name(fat_fs_t* fs, uint32_t dir_cluster, const char* name_83, fat_dirent_t* out_entry) {
    uint32_t count = fat_read_dir(fs, dir_cluster, fs->dir_scan_entries, 128);
    for (uint32_t k = 0; k < count; k++) {
        char dir_name[12];
        for (int j = 0; j < 11; j++) dir_name[j] = to_upper(fs->dir_scan_entries[k].name[j]);
        if (memcmp(dir_name, name_83, 11) == 0) {
            if (out_entry) *out_entry = fs->dir_scan_entries[k];
            return true;
        }
    }
    return false;
}

/* 找同名项或空闲槽（0x00/0xE5），位置经 out_lba/out_offset 返回。
 * 返回 1=同名项，2=空闲槽，0=目录满（需 dir_extend）。 */
static int dir_find_slot(fat_fs_t* fs, uint32_t dir_cluster, const char* name_83,
                         uint32_t* out_lba, uint32_t* out_offset) {
    uint32_t cluster = dir_cluster;
    if (dir_cluster == 0 && fs->fs_type == FAT32) cluster = fs->root_cluster;

    while (1) {
        uint32_t lba, sector_count;
        if (cluster == 0) {
            lba = fs->bpb.reserved_sectors + fs->bpb.num_fats * fs->fat_size;
            sector_count = fs->root_dir_sectors;
        } else {
            if (cluster < 2) break;
            lba = fs->first_data_sector + (cluster - 2) * fs->sectors_per_cluster;
            sector_count = fs->sectors_per_cluster;
        }

        for (uint32_t s = 0; s < sector_count; s++) {
            if (!fat_dev_read(fs, lba + s, fs->sector_buffer)) return 0;
            fat_dirent_t* d = (fat_dirent_t*)fs->sector_buffer;
            for (uint32_t i = 0; i < fs->bytes_per_sector / 32; i++) {
                if (d[i].name[0] == 0x00 || (uint8_t)d[i].name[0] == 0xE5) {
                    *out_lba = lba + s; *out_offset = i * 32;
                    return 2;   /* 空闲槽 */
                }
                char dn[12];
                for (int j = 0; j < 11; j++) dn[j] = to_upper(d[i].name[j]);
                if (memcmp(dn, name_83, 11) == 0) {
                    *out_lba = lba + s; *out_offset = i * 32;
                    return 1;   /* 同名项 */
                }
            }
        }
        if (cluster == 0) break;
        uint32_t next = read_fat_entry(fs, cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    return 0;   /* 目录满 */
}

/* 写 FAT 表项；FAT1 写完后同步镜像到 FAT2 对应扇区 */
static void write_fat_entry(fat_fs_t* fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset, fat_sector, offset;

    if (fs->fs_type == FAT16) {
        fat_offset = cluster * 2;
        fat_sector = fs->bpb.reserved_sectors + fat_offset / fs->bytes_per_sector;
        offset = fat_offset % fs->bytes_per_sector;
        if (!fat_dev_read(fs, fat_sector, fs->sector_buffer)) return;
        *(uint16_t*)(fs->sector_buffer + offset) = (uint16_t)(value & 0xFFFF);
        fat_dev_write(fs, fat_sector, fs->sector_buffer);
        fat_dev_write(fs, fat_sector + fs->fat_size, fs->sector_buffer);   /* 镜像 FAT2 */

    } else if (fs->fs_type == FAT32) {
        fat_offset = cluster * 4;
        fat_sector = fs->bpb.reserved_sectors + fat_offset / fs->bytes_per_sector;
        offset = fat_offset % fs->bytes_per_sector;
        if (!fat_dev_read(fs, fat_sector, fs->sector_buffer)) return;
        *(uint32_t*)(fs->sector_buffer + offset) =
            (*(uint32_t*)(fs->sector_buffer + offset) & 0xF0000000) | (value & 0x0FFFFFFF);
        fat_dev_write(fs, fat_sector, fs->sector_buffer);
        fat_dev_write(fs, fat_sector + fs->fat_size, fs->sector_buffer);

    } else if (fs->fs_type == FAT12) {
        fat_offset = cluster * 3 / 2;
        fat_sector = fs->bpb.reserved_sectors + fat_offset / fs->bytes_per_sector;
        offset = fat_offset % fs->bytes_per_sector;
        /* ⚠️ 同 read_fat_entry：FAT12 项可能跨扇区边界，
         * 旧实现 *(uint16_t*)(sector_buffer+511) 越界写。 */
        uint8_t b0, b1, old_lo = 0, old_hi = 0;
        if (offset + 1 < fs->bytes_per_sector) {
            if (!fat_dev_read(fs, fat_sector, fs->sector_buffer)) return;
            b0 = fs->sector_buffer[offset];
            b1 = fs->sector_buffer[offset + 1];
        } else {
            if (!fat_dev_read(fs, fat_sector, fs->sector_buffer)) return;
            b0 = fs->sector_buffer[offset];
            old_hi = fs->sector_buffer[offset + 1];   /* 越界值丢弃（下一扇区头） */
            if (!fat_dev_read(fs, fat_sector + 1, fs->sector_buffer)) return;
            b1 = fs->sector_buffer[0];
            old_lo = fs->sector_buffer[1];
        }
        uint16_t v = (uint16_t)(b0 | (b1 << 8));
        if (cluster & 1)
            v = (uint16_t)((v & 0x000F) | ((value & 0x0FFF) << 4));
        else
            v = (uint16_t)((v & 0xF000) | (value & 0x0FFF));
        if (offset + 1 < fs->bytes_per_sector) {
            fs->sector_buffer[offset]     = (uint8_t)(v & 0xFF);
            fs->sector_buffer[offset + 1] = (uint8_t)((v >> 8) & 0xFF);
            fat_dev_write(fs, fat_sector, fs->sector_buffer);
            fat_dev_write(fs, fat_sector + fs->fat_size, fs->sector_buffer);
        } else {
            /* 跨扇区：低字节写本扇区尾，高字节写下一扇区头 */
            fs->sector_buffer[offset] = (uint8_t)(v & 0xFF);
            fat_dev_write(fs, fat_sector, fs->sector_buffer);
            fat_dev_write(fs, fat_sector + fs->fat_size, fs->sector_buffer);
            if (!fat_dev_read(fs, fat_sector + 1, fs->sector_buffer)) return;
            fs->sector_buffer[0] = (uint8_t)((v >> 8) & 0xFF);
            (void)old_lo; (void)old_hi;
            fat_dev_write(fs, fat_sector + 1, fs->sector_buffer);
            fat_dev_write(fs, fat_sector + 1 + fs->fat_size, fs->sector_buffer);
        }
    }
}

/* 目录簇链尾接一个新簇，返回新簇首扇区 LBA */
static bool dir_extend(fat_fs_t* fs, uint32_t dir_cluster, uint32_t* new_lba) {
    /* 找链尾 */
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0;
    while (cluster >= 2 && guard++ < 65536) {
        uint32_t next = read_fat_entry(fs, cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    if (cluster < 2) return false;

    uint32_t data_sectors = fs->total_sectors - (fs->bpb.reserved_sectors + fs->bpb.num_fats * fs->fat_size + fs->root_dir_sectors);
    uint32_t max_cluster = 2 + data_sectors / fs->sectors_per_cluster;

    for (uint32_t c = 2; c < max_cluster; c++) {
        if (read_fat_entry(fs, c) == 0x0000) {
            write_fat_entry(fs, cluster, c);       /* 链尾 → 新簇 */
            write_fat_entry(fs, c, 0xFFF8);        /* 新簇成为新链尾 */
            uint32_t lba = fs->first_data_sector + (c - 2) * fs->sectors_per_cluster;
            for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
                memset(fs->sector_buffer, 0, fs->bytes_per_sector);
                fat_dev_write(fs, lba + s, fs->sector_buffer);
            }
            *new_lba = lba;
            return true;
        }
    }
    return false;   /* 磁盘满 */
}

/* 路径解析：把 "DIR1/DIR2/FILE" 拆成父目录簇号 + 末段 8.3 名。
 * 返回父目录簇号（0=根）；末段存在与否写入 *exists；
 * 中间段缺失/不是目录返回 0xFFFFFFFF。 */
static uint32_t fat_resolve_parent(fat_fs_t* fs, const char* path, char out_name_83[12],
                                   fat_dirent_t* out_entry, bool* exists) {
    if (!path || path[0] == '\0') return 0xFFFFFFFF;

    char seg[16];
    uint32_t cur = 0;          /* 当前所在目录簇（0=根） */
    uint32_t seg_len = 0;
    const char* p = path;
    bool have_seg = false;

    while (1) {
        if (*p == '/' || *p == '\0') {
            if (have_seg) {
                seg[seg_len] = '\0';
                char name_83[12];
                make_83_name(seg, name_83);

                fat_dirent_t entry;
                if (dir_find_name(fs, cur, name_83, &entry)) {
                    if (*p == '\0') {
                        /* 末段已存在 */
                        memcpy(out_name_83, name_83, 12);
                        *out_entry = entry;
                        *exists = true;
                        return cur;
                    }
                    /* 中间段：必须是目录才能继续深入 */
                    if (!(entry.attributes & 0x10)) return 0xFFFFFFFF;
                    cur = (entry.cluster_high << 16) | entry.cluster_low;
                } else {
                    if (*p == '\0') {
                        /* 末段不存在（新建场景） */
                        memcpy(out_name_83, name_83, 12);
                        *exists = false;
                        return cur;
                    }
                    return 0xFFFFFFFF;   /* 中间段不存在 */
                }
                seg_len = 0;
                have_seg = false;
            }
            if (*p == '\0') break;
            p++;
            continue;
        }
        if (seg_len < sizeof(seg) - 1) seg[seg_len++] = *p;
        have_seg = true;
        p++;
    }
    return 0xFFFFFFFF;   /* 空路径 */
}

/* 解析目录路径，返回目录首簇号（根=0）；不存在/不是目录返回 0xFFFFFFFF */
uint32_t fat_open_dir(fat_fs_t* fs, const char* path) {
    uint32_t fl = irq_lock();
    if (!path || path[0] == '\0') { irq_unlock(fl); return 0; }

    char seg[16];
    uint32_t cur = 0;
    uint32_t seg_len = 0;
    const char* p = path;
    bool have_seg = false;

    while (1) {
        if (*p == '/' || *p == '\0') {
            if (have_seg) {
                seg[seg_len] = '\0';
                char name_83[12];
                make_83_name(seg, name_83);
                fat_dirent_t entry;
                if (!dir_find_name(fs, cur, name_83, &entry)) { irq_unlock(fl); return 0xFFFFFFFF; }
                if (!(entry.attributes & 0x10)) { irq_unlock(fl); return 0xFFFFFFFF; }   /* 不是目录 */
                cur = (entry.cluster_high << 16) | entry.cluster_low;
                seg_len = 0;
                have_seg = false;
            }
            if (*p == '\0') break;
            p++;
            continue;
        }
        if (seg_len < sizeof(seg) - 1) seg[seg_len++] = *p;
        have_seg = true;
        p++;
    }
    irq_unlock(fl);
    return cur;
}

/* 查找文件（支持 "DIR/FILE"）：路径解析 + 末段 8.3 名匹配 */
bool fat_find_file(fat_fs_t* fs, const char* filename, fat_dirent_t* out_entry) {
    uint32_t fl = irq_lock();
    if (!fs->initialized || !filename || !out_entry) { irq_unlock(fl); return false; }

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(fs, filename, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF || !exists) { irq_unlock(fl); return false; }
    *out_entry = entry;
    irq_unlock(fl);
    return true;
}

/* ---- Linux 权限 meta：目录项借用字段的读写 ---- */

void fat_entry_unix_meta(const fat_dirent_t* e, uint16_t* mode, uint16_t* uid, uint16_t* gid) {
    if (!e) { if (mode) *mode = S_IFREG | 0644; if (uid) *uid = 0; if (gid) *gid = 0; return; }
    if ((e->creation_time >> 8) == FAT_META_MAGIC) {
        if (mode) *mode = (uint16_t)((uint16_t)e->creation_time_tenths |
                                     ((uint16_t)(e->creation_time & 0xFF) << 8));
        if (uid)  *uid  = e->creation_date;
        if (gid)  *gid  = e->last_access_date;
    } else {
        /* 旧条目/无 meta：按类型给默认值 */
        if (mode) *mode = (e->attributes & 0x10) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        if (uid)  *uid  = 0;
        if (gid)  *gid  = 0;
    }
}

void fat_entry_set_unix_meta(fat_dirent_t* e, uint16_t mode, uint16_t uid, uint16_t gid) {
    if (!e) return;
    e->creation_time_tenths = (uint8_t)(mode & 0xFF);
    e->creation_time = (uint16_t)(((mode >> 8) & 0xFF) | (FAT_META_MAGIC << 8));
    e->creation_date = uid;
    e->last_access_date = gid;
}

bool fat_get_file_meta(fat_fs_t* fs, const char* filename, uint16_t* mode, uint16_t* uid, uint16_t* gid) {
    uint32_t fl = irq_lock();
    if (!fs || !fs->initialized || !filename) { irq_unlock(fl); return false; }
    fat_dirent_t entry;
    if (!fat_find_file(fs, filename, &entry)) { irq_unlock(fl); return false; }
    fat_entry_unix_meta(&entry, mode, uid, gid);
    irq_unlock(fl);
    return true;
}

bool fat_set_file_meta(fat_fs_t* fs, const char* filename, uint16_t mode, uint16_t uid, uint16_t gid) {
    uint32_t fl = irq_lock();
    if (!fs || !fs->initialized || !filename) { irq_unlock(fl); return false; }

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(fs, filename, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF || !exists) { irq_unlock(fl); return false; }

    uint32_t lba, offset;
    int slot = dir_find_slot(fs, parent, name_83, &lba, &offset);
    if (slot != 1) { irq_unlock(fl); return false; }
    if (!fat_dev_read(fs, lba, fs->sector_buffer)) { irq_unlock(fl); return false; }
    fat_dirent_t* e = (fat_dirent_t*)(fs->sector_buffer + offset);
    fat_entry_set_unix_meta(e, mode, uid, gid);
    bool ok = fat_dev_write(fs, lba, fs->sector_buffer);
    irq_unlock(fl);
    return ok;
}

/* 沿簇链读文件：首簇 → FAT[next] → ... → EOF。
 * 簇 N 的扇区 = first_data_sector + (N-2) * sectors_per_cluster */
uint32_t fat_load_file(fat_fs_t* fs, const fat_dirent_t* entry, void* buffer, uint32_t max_size) {
    uint32_t fl = irq_lock();
    if (!fs->initialized || !entry || !buffer || max_size == 0) { irq_unlock(fl); return 0; }

    uint32_t first_cluster = (entry->cluster_high << 16) | entry->cluster_low;
    uint32_t file_size = entry->file_size;
    if (file_size > max_size) file_size = max_size;

    uint8_t* dest = (uint8_t*)buffer;
    uint32_t remaining = file_size;
    uint32_t cluster = first_cluster;
    if (cluster < 2) { irq_unlock(fl); return 0; }

    while (remaining > 0 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fs->first_data_sector + (cluster - 2) * fs->sectors_per_cluster;
        for (uint32_t s = 0; s < fs->sectors_per_cluster && remaining > 0; s++) {
            if (!fat_dev_read(fs, lba + s, fs->sector_buffer)) {
                irq_unlock(fl);
                return file_size - remaining;
            }
            uint32_t copy = (remaining < fs->bytes_per_sector) ? remaining : fs->bytes_per_sector;
            memcpy(dest, fs->sector_buffer, copy);
            dest += copy;
            remaining -= copy;
        }
        uint32_t next = read_fat_entry(fs, cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    irq_unlock(fl);
    return file_size - remaining;
}

/* 从文件偏移 offset 处读最多 max_size 字节到 buffer。
 * 遍历簇链跳过前 offset 字节，然后顺序读取。返回实际字节数。
 * ⚠️ fat_load_file 只能从头读；TUI 文件管理器分块复制大文件需要它。 */
uint32_t fat_load_file_off(fat_fs_t* fs, const fat_dirent_t* entry,
                           uint32_t offset, void* buffer, uint32_t max_size) {
    uint32_t fl = irq_lock();
    if (!fs->initialized || !entry || !buffer || max_size == 0) { irq_unlock(fl); return 0; }
    if (offset >= entry->file_size) { irq_unlock(fl); return 0; }

    uint32_t first_cluster = (entry->cluster_high << 16) | entry->cluster_low;
    uint32_t file_size = entry->file_size;
    if (file_size - offset < max_size) max_size = file_size - offset;

    uint8_t* dest = (uint8_t*)buffer;
    uint32_t remaining = max_size;
    uint32_t cluster = first_cluster;
    uint32_t skip = offset;
    if (cluster < 2) { irq_unlock(fl); return 0; }

    while (remaining > 0 && cluster < 0x0FFFFFF8) {
        uint32_t lba = fs->first_data_sector + (cluster - 2) * fs->sectors_per_cluster;
        for (uint32_t s = 0; s < fs->sectors_per_cluster && remaining > 0; s++) {
            if (!fat_dev_read(fs, lba + s, fs->sector_buffer)) {
                irq_unlock(fl);
                return max_size - remaining;
            }
            uint32_t off_in_sector = 0;
            if (skip > 0) {
                /* 跳过本扇区内的前 skip 字节 */
                if (skip >= fs->bytes_per_sector) {
                    skip -= fs->bytes_per_sector;
                    continue;
                }
                off_in_sector = skip;
                skip = 0;
            }
            uint32_t avail = fs->bytes_per_sector - off_in_sector;
            uint32_t copy = (remaining < avail) ? remaining : avail;
            memcpy(dest, fs->sector_buffer + off_in_sector, copy);
            dest += copy;
            remaining -= copy;
        }
        uint32_t next = read_fat_entry(fs, cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    irq_unlock(fl);
    return max_size - remaining;
}

/* 整条簇链表项写 0，含 FAT2 镜像 */
static void fat_free_chain(fat_fs_t* fs, uint32_t first_cluster) {
    uint32_t c = first_cluster;
    uint32_t guard = 0;
    while (c >= 2 && c < 0x0FFFFFF8 && guard++ < 65536) {
        uint32_t next = read_fat_entry(fs, c);
        write_fat_entry(fs, c, 0x0000);
        if (next >= 0x0FFFFFF8) break;
        c = next;
    }
}

/* 删除文件或空目录：标记 0xE5 + 释放簇链。目录只有空（仅 '.'/'..'）才能删 */
bool fat_delete_file(fat_fs_t* fs, const char* filename) {
    if (!fs->initialized || !filename) return false;

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(fs, filename, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF || !exists) return false;

    uint32_t lba, offset;
    int slot = dir_find_slot(fs, parent, name_83, &lba, &offset);
    if (slot != 1) return false;

    if (!fat_dev_read(fs, lba, fs->sector_buffer)) return false;
    fat_dirent_t* e = (fat_dirent_t*)(fs->sector_buffer + offset);

    /* 先拷出需要的数据——sector_buffer 会被后续调用污染 */
    uint32_t first_cluster = (e->cluster_high << 16) | e->cluster_low;
    bool is_dir = (e->attributes & 0x10) != 0;

    /* 目录：先查是否为空。fat_read_dir 会复用 sector_buffer，
     * 所以空检查必须在改目录项之前，之后要重新读扇区。 */
    if (is_dir && first_cluster >= 2) {
        uint32_t n = fat_read_dir(fs, first_cluster, fs->dir_scan_entries, 128);
        for (uint32_t i = 0; i < n; i++) {
            if (fs->dir_scan_entries[i].name[0] == 0x2E) continue;   /* 跳过 '.' 和 '..' */
            return false;   /* 非空目录，拒绝删除 */
        }
    }

    /* 重新读目录项所在扇区（sector_buffer 可能已被 fat_read_dir 污染） */
    if (!fat_dev_read(fs, lba, fs->sector_buffer)) return false;
    e = (fat_dirent_t*)(fs->sector_buffer + offset);
    e->name[0] = 0xE5;   /* 标记已删除 */
    if (!fat_dev_write(fs, lba, fs->sector_buffer)) return false;
    if (first_cluster >= 2) fat_free_chain(fs, first_cluster);
    return true;
}

/* 新建/覆盖文件：找空闲项（或同名覆盖）→ 释放旧链（覆盖时）→
 * 分配新簇链 → 写 FAT（含镜像）→ 写数据 → 填目录项 */
static bool fat_write_file_impl(fat_fs_t* fs, const char* filename, const void* data, uint32_t size) {
    if (!fs->initialized || !filename || !data) return false;

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(fs, filename, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF) return false;

    uint32_t data_sectors = fs->total_sectors - (fs->bpb.reserved_sectors + fs->bpb.num_fats * fs->fat_size + fs->root_dir_sectors);
    uint32_t max_cluster = 2 + data_sectors / fs->sectors_per_cluster;

    /* 找空闲项或同名项；目录满则扩展 */
    uint32_t entry_sector = 0, entry_offset = 0;
    bool overwriting = false;
    uint32_t old_first_cluster = 0;

    int slot = dir_find_slot(fs, parent, name_83, &entry_sector, &entry_offset);
    if (slot == 0) {
        /* 目录满：扩展一簇（根目录固定区无法扩展） */
        if (parent == 0) return false;
        uint32_t new_lba;
        if (!dir_extend(fs, parent, &new_lba)) return false;
        entry_sector = new_lba;
        entry_offset = 0;
    } else if (slot == 1) {
        if (!fat_dev_read(fs, entry_sector, fs->sector_buffer)) return false;
        fat_dirent_t* e = (fat_dirent_t*)(fs->sector_buffer + entry_offset);
        if (e->attributes & 0x10) return false;   /* 不能覆盖目录 */
        old_first_cluster = (e->cluster_high << 16) | e->cluster_low;
        overwriting = true;
    }

    /* 计算所需簇数并扫描空闲簇构建簇链 */
    uint32_t cluster_bytes = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t needed = (size + cluster_bytes - 1) / cluster_bytes;
    uint32_t first_cluster = 0, prev_cluster = 0, allocated = 0;

    for (uint32_t c = 2; c < max_cluster && allocated < needed; c++) {
        if (read_fat_entry(fs, c) == 0x0000) {
            if (!first_cluster) first_cluster = c;
            if (prev_cluster) write_fat_entry(fs, prev_cluster, c);
            prev_cluster = c;
            allocated++;
        }
    }
    if (allocated < needed) {
        fat_free_chain(fs, first_cluster);
        return false;
    }
    if (prev_cluster) write_fat_entry(fs, prev_cluster, 0xFFF8);   /* 链尾标记 */

    /* 覆盖时释放旧链（在新链分配成功之后，避免中途失败丢数据） */
    if (overwriting && old_first_cluster >= 2) fat_free_chain(fs, old_first_cluster);

    /* 写数据扇区（最后一簇不足的部分补零） */
    const uint8_t* src = (const uint8_t*)data;
    uint32_t remaining = size;
    uint32_t c = first_cluster;
    while (remaining > 0 && c) {
        uint32_t lba = fs->first_data_sector + (c - 2) * fs->sectors_per_cluster;
        for (uint32_t s = 0; s < fs->sectors_per_cluster && remaining > 0; s++) {
            memset(fs->sector_buffer, 0, fs->bytes_per_sector);
            uint32_t copy = (remaining < fs->bytes_per_sector) ? remaining : fs->bytes_per_sector;
            memcpy(fs->sector_buffer, src, copy);
            if (!fat_dev_write(fs, lba + s, fs->sector_buffer)) {
                /* ⚠️ 写扇区失败时释放已分配的簇链，避免磁盘空间永久泄漏 */
                fat_free_chain(fs, first_cluster);
                return false;
            }
            src += copy;
            remaining -= copy;
        }
        uint32_t next = read_fat_entry(fs, c);
        if (next >= 0x0FFFFFF8) break;
        c = next;
    }

    /* 填充目录项并写回 */
    if (!fat_dev_read(fs, entry_sector, fs->sector_buffer)) return false;
    fat_dirent_t* e = (fat_dirent_t*)(fs->sector_buffer + entry_offset);
    for (int i = 0; i < 11; i++) e->name[i] = (uint8_t)name_83[i];
    e->attributes = 0x20;                  /* 归档属性 */
    e->nt_reserved = 0;
    e->creation_time_tenths = 0;
    e->creation_time = 0;
    e->creation_date = 0;
    e->last_access_date = 0;
    e->cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
    e->last_write_time = 0;
    e->last_write_date = 0;
    e->cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    e->file_size = size;
    if (!fat_dev_write(fs, entry_sector, fs->sector_buffer)) return false;

    return true;
}

/* ⚠️ 架构升级（内核态抢占）：锁包装 */
bool fat_write_file(fat_fs_t* fs, const char* filename, const void* data, uint32_t size) {
    uint32_t fl = irq_lock();
    bool r = fat_write_file_impl(fs, filename, data, size);
    irq_unlock(fl);
    return r;
}

/* 建子目录：分配一簇清零 → 写 '.'（指向自己）'..'（指向父）→
 * 父目录找空闲项（满则扩展）→ 填目录项（属性 0x10） */
static bool fat_mkdir_impl(fat_fs_t* fs, const char* path) {
    if (!fs->initialized || !path) return false;

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(fs, path, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF || exists) return false;

    uint32_t data_sectors = fs->total_sectors - (fs->bpb.reserved_sectors + fs->bpb.num_fats * fs->fat_size + fs->root_dir_sectors);
    uint32_t max_cluster = 2 + data_sectors / fs->sectors_per_cluster;

    /* 分配一个新簇存放目录内容 */
    uint32_t new_cluster = 0;
    for (uint32_t c = 2; c < max_cluster; c++) {
        if (read_fat_entry(fs, c) == 0x0000) { new_cluster = c; break; }
    }
    if (new_cluster == 0) return false;   /* 磁盘满 */

    write_fat_entry(fs, new_cluster, 0xFFF8);   /* 单簇目录，链尾 */
    uint32_t lba = fs->first_data_sector + (new_cluster - 2) * fs->sectors_per_cluster;
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        memset(fs->sector_buffer, 0, fs->bytes_per_sector);
        fat_dev_write(fs, lba + s, fs->sector_buffer);
    }

    /* 写 '.' 和 '..' */
    fat_dirent_t dot;
    memset(&dot, 0, sizeof(dot));
    for (int i = 0; i < 11; i++) dot.name[i] = (i == 0) ? 0x2E : 0x20;   /* ".          " */
    dot.attributes = 0x10;
    dot.cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    dot.cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);

    fat_dirent_t dotdot = dot;
    dotdot.name[0] = 0x2E;
    dotdot.name[1] = 0x2E;
    for (int i = 2; i < 11; i++) dotdot.name[i] = 0x20;                  /* "..         " */
    uint32_t parent_cluster = (parent == 0 && fs->fs_type == FAT32) ? 0x0FFFFFF8 : parent;
    dotdot.cluster_low = (uint16_t)(parent_cluster & 0xFFFF);
    dotdot.cluster_high = (uint16_t)((parent_cluster >> 16) & 0xFFFF);

    /* ⚠️ 以下各失败点统一释放 new_cluster，避免泄漏 */
    if (!fat_dev_read(fs, lba, fs->sector_buffer)) { fat_free_chain(fs, new_cluster); return false; }
    memcpy(fs->sector_buffer, &dot, sizeof(dot));
    memcpy(fs->sector_buffer + 32, &dotdot, sizeof(dotdot));
    if (!fat_dev_write(fs, lba, fs->sector_buffer)) { fat_free_chain(fs, new_cluster); return false; }

    /* 在父目录写新目录项（满则扩展） */
    uint32_t entry_sector, entry_offset;
    int slot = dir_find_slot(fs, parent, name_83, &entry_sector, &entry_offset);
    if (slot == 0) {
        if (parent == 0) { fat_free_chain(fs, new_cluster); return false; }   /* 根目录固定区满 */
        uint32_t new_lba;
        if (!dir_extend(fs, parent, &new_lba)) { fat_free_chain(fs, new_cluster); return false; }
        entry_sector = new_lba;
        entry_offset = 0;
    } else if (slot == 1) {
        fat_free_chain(fs, new_cluster);   /* 同名已存在 */
        return false;
    }

    if (!fat_dev_read(fs, entry_sector, fs->sector_buffer)) { fat_free_chain(fs, new_cluster); return false; }
    fat_dirent_t* e = (fat_dirent_t*)(fs->sector_buffer + entry_offset);
    for (int i = 0; i < 11; i++) e->name[i] = (uint8_t)name_83[i];
    e->attributes = 0x10;                  /* 目录属性 */
    e->nt_reserved = 0;
    e->creation_time_tenths = 0;
    e->creation_time = 0;
    e->creation_date = 0;
    e->last_access_date = 0;
    e->cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);
    e->last_write_time = 0;
    e->last_write_date = 0;
    e->cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    e->file_size = 0;                      /* 目录大小为 0 */
    if (!fat_dev_write(fs, entry_sector, fs->sector_buffer)) { fat_free_chain(fs, new_cluster); return false; }

    return true;
}

/* ⚠️ 架构升级（内核态抢占）：锁包装 */
bool fat_mkdir(fat_fs_t* fs, const char* path) {
    uint32_t fl = irq_lock();
    bool r = fat_mkdir_impl(fs, path);
    irq_unlock(fl);
    return r;
}
