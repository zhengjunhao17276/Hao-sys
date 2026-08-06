/*
 * fat.c - FAT12/16/32 文件系统驱动
 * BPB 解析、类型检测、簇链遍历、目录读写、8.3 文件名查找、
 * 文件加载/写入、目录创建/删除。
 */

#include "../include/driver/ata.h"
#include "../include/driver/vga.h"
#include "../include/lib/string.h"
#include "../include/fs/fat.h"
#include <stdint.h>
#include <stdbool.h>
#include "../include/driver/irqlock.h"

/* 全局状态，fat_init() 里填 */
static fat_bpb_t bpb;
static uint32_t first_data_sector;       /* 数据区起始 LBA */
static uint32_t root_dir_sectors;        /* FAT12/16 根目录占用的扇区数 */
static uint32_t total_sectors;
static uint32_t fat_size;                /* 每份 FAT 的扇区数 */
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_sector;
static uint32_t root_dir_entries;
static fat_type_t fs_type = FAT_UNKNOWN;
static bool fat_initialized = false;
static uint32_t root_cluster = 0;        /* FAT32 根目录起始簇 */
static uint8_t sector_buffer[512];       /* 单扇区缓冲 */

static bool read_sector(uint32_t lba, void* buffer) {
    return ata_read_sector(lba, (uint8_t*)buffer);
}

static bool write_sector(uint32_t lba, const void* buffer) {
    return ata_write_sector(lba, (const uint8_t*)buffer);
}

/* FAT12 项 1.5 字节（奇偶簇取高/低 12 位）；FAT16 直接偏移；
 * FAT32 4 字节仅低 28 位有效 */
static uint32_t read_fat_entry(uint32_t cluster) {
    if (fs_type == FAT12) {
        uint32_t fat_offset = cluster * 3 / 2;
        uint32_t byte_off   = fat_offset % bytes_per_sector;
        uint32_t fat_sector = bpb.reserved_sectors + fat_offset / bytes_per_sector;
        /* ⚠️ FAT12 表项可能跨扇区边界（奇簇时低字节落在 511、高字节在
         * 下一扇区 0），旧实现 *(uint16_t*)(sector_buffer+511) 越界读。 */
        uint8_t b0, b1;
        if (byte_off + 1 < bytes_per_sector) {
            if (!read_sector(fat_sector, sector_buffer)) return 0x0FFFFFFF;
            b0 = sector_buffer[byte_off];
            b1 = sector_buffer[byte_off + 1];
        } else {
            if (!read_sector(fat_sector, sector_buffer)) return 0x0FFFFFFF;
            b0 = sector_buffer[byte_off];
            if (!read_sector(fat_sector + 1, sector_buffer)) return 0x0FFFFFFF;
            b1 = sector_buffer[0];
        }
        uint16_t val = (uint16_t)(b0 | (b1 << 8));
        if (cluster & 1) val >>= 4;
        else val &= 0x0FFF;
        return val;
    } else if (fs_type == FAT16) {
        uint32_t fat_offset = cluster * 2;
        uint32_t fat_sector = bpb.reserved_sectors + fat_offset / bytes_per_sector;
        uint32_t offset = fat_offset % bytes_per_sector;
        if (!read_sector(fat_sector, sector_buffer)) return 0xFFFF;
        return *(uint16_t*)(sector_buffer + offset);
    } else if (fs_type == FAT32) {
        uint32_t fat_offset = cluster * 4;
        uint32_t fat_sector = bpb.reserved_sectors + fat_offset / bytes_per_sector;
        uint32_t offset = fat_offset % bytes_per_sector;
        if (!read_sector(fat_sector, sector_buffer)) return 0x0FFFFFFF;
        return *(uint32_t*)(sector_buffer + offset) & 0x0FFFFFFF;
    }
    return 0x0FFFFFFF;
}

/* 微软规范阈值：总簇数 <4085 → FAT12，<65525 → FAT16，否则 FAT32 */
static fat_type_t detect_fat_type(void) {
    if (bpb.bytes_per_sector != 512) {
        vga_write("[FAT] Warning: bytes_per_sector != 512, may not be FAT.\n");
        return FAT_UNKNOWN;
    }
    if (bpb.sectors_per_cluster == 0 || bpb.sectors_per_cluster > 64) {
        vga_write("[FAT] Invalid sectors_per_cluster.\n");
        return FAT_UNKNOWN;
    }
    uint32_t root_dir_sectors_calc =
        ((bpb.root_entry_count * 32) + bpb.bytes_per_sector - 1) / bpb.bytes_per_sector;
    uint32_t fat_size_used = bpb.fat_size_16 ? bpb.fat_size_16 : bpb.fat_size_32;
    uint32_t total_sectors_used = bpb.total_sectors_16 ? bpb.total_sectors_16 : bpb.total_sectors_32;
    if (fat_size_used == 0) {
        vga_write("[FAT] FAT size is zero, cannot determine type.\n");
        return FAT_UNKNOWN;
    }
    uint32_t data_sectors =
        total_sectors_used - (bpb.reserved_sectors + bpb.num_fats * fat_size_used + root_dir_sectors_calc);
    uint32_t total_clusters = data_sectors / bpb.sectors_per_cluster;
    vga_write("[FAT] Detected total clusters: ");
    vga_write_hex(total_clusters);
    vga_write("\n");
    if (total_clusters < 4085) return FAT12;
    else if (total_clusters < 65525) return FAT16;
    else return FAT32;
}

/* 读 BPB、验 0x55AA 签名、定 FAT 类型（字符串优先，数值推断兜底） */
bool fat_init(void) {
    if (!read_sector(0, sector_buffer)) {
        vga_write("[FAT] Failed to read boot sector.\n");
        return false;
    }
    fat_bpb_t* b = (fat_bpb_t*)sector_buffer;
    bpb = *b;

    vga_write("[FAT] BPB: bytes_per_sector=");
    vga_write_hex(bpb.bytes_per_sector);
    vga_write(" sectors_per_cluster=");
    vga_write_hex(bpb.sectors_per_cluster);
    vga_write(" reserved_sectors=");
    vga_write_hex(bpb.reserved_sectors);
    vga_write(" num_fats=");
    vga_write_hex(bpb.num_fats);
    vga_write(" root_entry_count=");
    vga_write_hex(bpb.root_entry_count);
    vga_write(" total_sectors_16=");
    vga_write_hex(bpb.total_sectors_16);
    vga_write(" total_sectors_32=");
    vga_write_hex(bpb.total_sectors_32);
    vga_write(" fat_size_16=");
    vga_write_hex(bpb.fat_size_16);
    vga_write(" fat_size_32=");
    vga_write_hex(bpb.fat_size_32);
    vga_write(" fs_type='");
    for (int i=0; i<8; i++) vga_putchar(bpb.fs_type[i]);
    vga_write("'\n");

    if (sector_buffer[510] != 0x55 || sector_buffer[511] != 0xAA) {
        vga_write("[FAT] Invalid boot sector signature (0x55AA missing).\n");
        return false;
    }

    fs_type = FAT_UNKNOWN;
    if (memcmp(bpb.fs_type, "FAT12", 5) == 0) fs_type = FAT12;
    else if (memcmp(bpb.fs_type, "FAT16", 5) == 0) fs_type = FAT16;
    else if (memcmp(bpb.fs_type, "FAT32", 5) == 0) fs_type = FAT32;

    if (fs_type == FAT_UNKNOWN) {
        vga_write("[FAT] String match failed, using numerical detection.\n");
        fs_type = detect_fat_type();
    }
    if (fs_type == FAT_UNKNOWN) {
        vga_write("[FAT] Unrecognized filesystem.\n");
        return false;
    }

    bytes_per_sector    = bpb.bytes_per_sector;
    sectors_per_cluster = bpb.sectors_per_cluster;
    root_dir_entries    = bpb.root_entry_count;
    root_dir_sectors    = ((root_dir_entries * 32) + bytes_per_sector - 1) / bytes_per_sector;
    total_sectors       = bpb.total_sectors_16 ? bpb.total_sectors_16 : bpb.total_sectors_32;
    fat_size            = bpb.fat_size_16 ? bpb.fat_size_16 : bpb.fat_size_32;
    first_data_sector   = bpb.reserved_sectors + bpb.num_fats * fat_size + root_dir_sectors;

    if (fs_type == FAT32) {
        root_cluster      = bpb.root_cluster;
        root_dir_sectors  = 0;
        first_data_sector = bpb.reserved_sectors + bpb.num_fats * fat_size;
    }

    fat_initialized = true;
    vga_write("[FAT] Detected ");
    if (fs_type == FAT12) vga_write("FAT12");
    else if (fs_type == FAT16) vga_write("FAT16");
    else if (fs_type == FAT32) vga_write("FAT32");
    vga_write(" filesystem.\n");
    return true;
}

fat_type_t fat_get_type(void) {
    uint32_t fl = irq_lock();
    fat_type_t t = fs_type;
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
uint32_t fat_read_dir(uint32_t dir_cluster, fat_dirent_t* entries, uint32_t max_entries) {
    uint32_t fl = irq_lock();
    if (!fat_initialized || !entries || max_entries == 0) { irq_unlock(fl); return 0; }

    uint32_t index = 0;
    uint32_t cluster = dir_cluster;
    if (dir_cluster == 0 && fs_type == FAT32) cluster = root_cluster;

    while (1) {
        uint32_t lba, sector_count;
        if (cluster == 0) {
            /* FAT12/16 根目录固定区 */
            lba = bpb.reserved_sectors + bpb.num_fats * fat_size;
            sector_count = root_dir_sectors;
        } else {
            if (cluster < 2) break;
            lba = first_data_sector + (cluster - 2) * sectors_per_cluster;
            sector_count = sectors_per_cluster;
        }

        for (uint32_t s = 0; s < sector_count; s++) {
            if (!read_sector(lba + s, sector_buffer)) break;
            fat_dirent_t* dirent = (fat_dirent_t*)sector_buffer;
            uint32_t per_sector = bytes_per_sector / 32;
            for (uint32_t i = 0; i < per_sector && index < max_entries; i++) {
                if (dirent[i].name[0] == 0x00) { irq_unlock(fl); return index; }
                if ((uint8_t)dirent[i].name[0] == 0xE5) continue;
                entries[index++] = dirent[i];
            }
        }

        if (cluster == 0) break;   /* 根目录固定区读完 */
        uint32_t next = read_fat_entry(cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    irq_unlock(fl);
    return index;
}

/* 根目录条目（兼容包装） */
uint32_t fat_read_root_dir(fat_dirent_t* entries, uint32_t max_entries) {
    return fat_read_dir(0, entries, max_entries);
}

/* ⚠️ 目录扫描缓冲绝不能放栈上：128 项 = 4KB，而每个任务的内核栈只有
 * 4KB，栈数组会写穿到栈页之下（PCB 页），是定时炸弹（实测侥幸未炸）。
 * 内核单线程，全局缓冲安全。 */
static fat_dirent_t dir_scan_entries[128];

static bool dir_find_name(uint32_t dir_cluster, const char* name_83, fat_dirent_t* out_entry) {
    uint32_t count = fat_read_dir(dir_cluster, dir_scan_entries, 128);
    for (uint32_t k = 0; k < count; k++) {
        char dir_name[12];
        for (int j = 0; j < 11; j++) dir_name[j] = to_upper(dir_scan_entries[k].name[j]);
        if (memcmp(dir_name, name_83, 11) == 0) {
            if (out_entry) *out_entry = dir_scan_entries[k];
            return true;
        }
    }
    return false;
}

/* 找同名项或空闲槽（0x00/0xE5），位置经 out_lba/out_offset 返回。
 * 返回 1=同名项，2=空闲槽，0=目录满（需 dir_extend）。 */
static int dir_find_slot(uint32_t dir_cluster, const char* name_83,
                         uint32_t* out_lba, uint32_t* out_offset) {
    uint32_t cluster = dir_cluster;
    if (dir_cluster == 0 && fs_type == FAT32) cluster = root_cluster;

    while (1) {
        uint32_t lba, sector_count;
        if (cluster == 0) {
            lba = bpb.reserved_sectors + bpb.num_fats * fat_size;
            sector_count = root_dir_sectors;
        } else {
            if (cluster < 2) break;
            lba = first_data_sector + (cluster - 2) * sectors_per_cluster;
            sector_count = sectors_per_cluster;
        }

        for (uint32_t s = 0; s < sector_count; s++) {
            if (!read_sector(lba + s, sector_buffer)) return 0;
            fat_dirent_t* d = (fat_dirent_t*)sector_buffer;
            for (uint32_t i = 0; i < bytes_per_sector / 32; i++) {
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
        uint32_t next = read_fat_entry(cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    return 0;   /* 目录满 */
}

/* 写 FAT 表项；FAT1 写完后同步镜像到 FAT2 对应扇区 */
static void write_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset, fat_sector, offset;

    if (fs_type == FAT16) {
        fat_offset = cluster * 2;
        fat_sector = bpb.reserved_sectors + fat_offset / bytes_per_sector;
        offset = fat_offset % bytes_per_sector;
        if (!read_sector(fat_sector, sector_buffer)) return;
        *(uint16_t*)(sector_buffer + offset) = (uint16_t)(value & 0xFFFF);
        write_sector(fat_sector, sector_buffer);
        write_sector(fat_sector + fat_size, sector_buffer);   /* 镜像 FAT2 */

    } else if (fs_type == FAT32) {
        fat_offset = cluster * 4;
        fat_sector = bpb.reserved_sectors + fat_offset / bytes_per_sector;
        offset = fat_offset % bytes_per_sector;
        if (!read_sector(fat_sector, sector_buffer)) return;
        *(uint32_t*)(sector_buffer + offset) =
            (*(uint32_t*)(sector_buffer + offset) & 0xF0000000) | (value & 0x0FFFFFFF);
        write_sector(fat_sector, sector_buffer);
        write_sector(fat_sector + fat_size, sector_buffer);

    } else if (fs_type == FAT12) {
        fat_offset = cluster * 3 / 2;
        fat_sector = bpb.reserved_sectors + fat_offset / bytes_per_sector;
        offset = fat_offset % bytes_per_sector;
        /* ⚠️ 同 read_fat_entry：FAT12 项可能跨扇区边界，
         * 旧实现 *(uint16_t*)(sector_buffer+511) 越界写。 */
        uint8_t b0, b1, old_lo = 0, old_hi = 0;
        if (offset + 1 < bytes_per_sector) {
            if (!read_sector(fat_sector, sector_buffer)) return;
            b0 = sector_buffer[offset];
            b1 = sector_buffer[offset + 1];
        } else {
            if (!read_sector(fat_sector, sector_buffer)) return;
            b0 = sector_buffer[offset];
            old_hi = sector_buffer[offset + 1];   /* 越界值丢弃（下一扇区头） */
            if (!read_sector(fat_sector + 1, sector_buffer)) return;
            b1 = sector_buffer[0];
            old_lo = sector_buffer[1];
        }
        uint16_t v = (uint16_t)(b0 | (b1 << 8));
        if (cluster & 1)
            v = (uint16_t)((v & 0x000F) | ((value & 0x0FFF) << 4));
        else
            v = (uint16_t)((v & 0xF000) | (value & 0x0FFF));
        if (offset + 1 < bytes_per_sector) {
            sector_buffer[offset]     = (uint8_t)(v & 0xFF);
            sector_buffer[offset + 1] = (uint8_t)((v >> 8) & 0xFF);
            write_sector(fat_sector, sector_buffer);
            write_sector(fat_sector + fat_size, sector_buffer);
        } else {
            /* 跨扇区：低字节写本扇区尾，高字节写下一扇区头 */
            sector_buffer[offset] = (uint8_t)(v & 0xFF);
            write_sector(fat_sector, sector_buffer);
            write_sector(fat_sector + fat_size, sector_buffer);
            if (!read_sector(fat_sector + 1, sector_buffer)) return;
            sector_buffer[0] = (uint8_t)((v >> 8) & 0xFF);
            (void)old_lo; (void)old_hi;
            write_sector(fat_sector + 1, sector_buffer);
            write_sector(fat_sector + 1 + fat_size, sector_buffer);
        }
    }
}

/* 目录簇链尾接一个新簇，返回新簇首扇区 LBA */
static bool dir_extend(uint32_t dir_cluster, uint32_t* new_lba) {
    /* 找链尾 */
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0;
    while (cluster >= 2 && guard++ < 65536) {
        uint32_t next = read_fat_entry(cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    if (cluster < 2) return false;

    uint32_t data_sectors = total_sectors - (bpb.reserved_sectors + bpb.num_fats * fat_size + root_dir_sectors);
    uint32_t max_cluster = 2 + data_sectors / sectors_per_cluster;

    for (uint32_t c = 2; c < max_cluster; c++) {
        if (read_fat_entry(c) == 0x0000) {
            write_fat_entry(cluster, c);       /* 链尾 → 新簇 */
            write_fat_entry(c, 0xFFF8);        /* 新簇成为新链尾 */
            uint32_t lba = first_data_sector + (c - 2) * sectors_per_cluster;
            for (uint32_t s = 0; s < sectors_per_cluster; s++) {
                memset(sector_buffer, 0, bytes_per_sector);
                write_sector(lba + s, sector_buffer);
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
static uint32_t fat_resolve_parent(const char* path, char out_name_83[12],
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
                if (dir_find_name(cur, name_83, &entry)) {
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
uint32_t fat_open_dir(const char* path) {
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
                if (!dir_find_name(cur, name_83, &entry)) { irq_unlock(fl); return 0xFFFFFFFF; }
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
bool fat_find_file(const char* filename, fat_dirent_t* out_entry) {
    uint32_t fl = irq_lock();
    if (!fat_initialized || !filename || !out_entry) { irq_unlock(fl); return false; }

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(filename, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF || !exists) { irq_unlock(fl); return false; }
    *out_entry = entry;
    irq_unlock(fl);
    return true;
}

/* 沿簇链读文件：首簇 → FAT[next] → ... → EOF。
 * 簇 N 的扇区 = first_data_sector + (N-2) * sectors_per_cluster */
uint32_t fat_load_file(const fat_dirent_t* entry, void* buffer, uint32_t max_size) {
    uint32_t fl = irq_lock();
    if (!fat_initialized || !entry || !buffer || max_size == 0) { irq_unlock(fl); return 0; }

    uint32_t first_cluster = (entry->cluster_high << 16) | entry->cluster_low;
    uint32_t file_size = entry->file_size;
    if (file_size > max_size) file_size = max_size;

    uint8_t* dest = (uint8_t*)buffer;
    uint32_t remaining = file_size;
    uint32_t cluster = first_cluster;
    if (cluster < 2) { irq_unlock(fl); return 0; }

    while (remaining > 0 && cluster < 0x0FFFFFF8) {
        uint32_t lba = first_data_sector + (cluster - 2) * sectors_per_cluster;
        for (uint32_t s = 0; s < sectors_per_cluster && remaining > 0; s++) {
            if (!read_sector(lba + s, sector_buffer)) {
                irq_unlock(fl);
                return file_size - remaining;
            }
            uint32_t copy = (remaining < bytes_per_sector) ? remaining : bytes_per_sector;
            memcpy(dest, sector_buffer, copy);
            dest += copy;
            remaining -= copy;
        }
        uint32_t next = read_fat_entry(cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
    }
    irq_unlock(fl);
    return file_size - remaining;
}

/* 整条簇链表项写 0，含 FAT2 镜像 */
static void fat_free_chain(uint32_t first_cluster) {
    uint32_t c = first_cluster;
    uint32_t guard = 0;
    while (c >= 2 && c < 0x0FFFFFF8 && guard++ < 65536) {
        uint32_t next = read_fat_entry(c);
        write_fat_entry(c, 0x0000);
        if (next >= 0x0FFFFFF8) break;
        c = next;
    }
}

/* 删除文件或空目录：标记 0xE5 + 释放簇链。目录只有空（仅 '.'/'..'）才能删 */
bool fat_delete_file(const char* filename) {
    if (!fat_initialized || !filename) return false;

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(filename, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF || !exists) return false;

    uint32_t lba, offset;
    int slot = dir_find_slot(parent, name_83, &lba, &offset);
    if (slot != 1) return false;

    if (!read_sector(lba, sector_buffer)) return false;
    fat_dirent_t* e = (fat_dirent_t*)(sector_buffer + offset);

    /* 先拷出需要的数据——sector_buffer 会被后续调用污染 */
    uint32_t first_cluster = (e->cluster_high << 16) | e->cluster_low;
    bool is_dir = (e->attributes & 0x10) != 0;

    /* 目录：先查是否为空。fat_read_dir 会复用 sector_buffer，
     * 所以空检查必须在改目录项之前，之后要重新读扇区。 */
    if (is_dir && first_cluster >= 2) {
        uint32_t n = fat_read_dir(first_cluster, dir_scan_entries, 128);
        for (uint32_t i = 0; i < n; i++) {
            if (dir_scan_entries[i].name[0] == 0x2E) continue;   /* 跳过 '.' 和 '..' */
            return false;   /* 非空目录，拒绝删除 */
        }
    }

    /* 重新读目录项所在扇区（sector_buffer 可能已被 fat_read_dir 污染） */
    if (!read_sector(lba, sector_buffer)) return false;
    e = (fat_dirent_t*)(sector_buffer + offset);
    e->name[0] = 0xE5;   /* 标记已删除 */
    if (!write_sector(lba, sector_buffer)) return false;
    if (first_cluster >= 2) fat_free_chain(first_cluster);
    return true;
}

/* 新建/覆盖文件：找空闲项（或同名覆盖）→ 释放旧链（覆盖时）→
 * 分配新簇链 → 写 FAT（含镜像）→ 写数据 → 填目录项 */
static bool fat_write_file_impl(const char* filename, const void* data, uint32_t size) {
    if (!fat_initialized || !filename || !data) return false;

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(filename, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF) return false;

    uint32_t data_sectors = total_sectors - (bpb.reserved_sectors + bpb.num_fats * fat_size + root_dir_sectors);
    uint32_t max_cluster = 2 + data_sectors / sectors_per_cluster;

    /* 找空闲项或同名项；目录满则扩展 */
    uint32_t entry_sector = 0, entry_offset = 0;
    bool overwriting = false;
    uint32_t old_first_cluster = 0;

    int slot = dir_find_slot(parent, name_83, &entry_sector, &entry_offset);
    if (slot == 0) {
        /* 目录满：扩展一簇（根目录固定区无法扩展） */
        if (parent == 0) return false;
        uint32_t new_lba;
        if (!dir_extend(parent, &new_lba)) return false;
        entry_sector = new_lba;
        entry_offset = 0;
    } else if (slot == 1) {
        if (!read_sector(entry_sector, sector_buffer)) return false;
        fat_dirent_t* e = (fat_dirent_t*)(sector_buffer + entry_offset);
        if (e->attributes & 0x10) return false;   /* 不能覆盖目录 */
        old_first_cluster = (e->cluster_high << 16) | e->cluster_low;
        overwriting = true;
    }

    /* 计算所需簇数并扫描空闲簇构建簇链 */
    uint32_t cluster_bytes = sectors_per_cluster * bytes_per_sector;
    uint32_t needed = (size + cluster_bytes - 1) / cluster_bytes;
    uint32_t first_cluster = 0, prev_cluster = 0, allocated = 0;

    for (uint32_t c = 2; c < max_cluster && allocated < needed; c++) {
        if (read_fat_entry(c) == 0x0000) {
            if (!first_cluster) first_cluster = c;
            if (prev_cluster) write_fat_entry(prev_cluster, c);
            prev_cluster = c;
            allocated++;
        }
    }
    if (allocated < needed) {
        fat_free_chain(first_cluster);
        return false;
    }
    if (prev_cluster) write_fat_entry(prev_cluster, 0xFFF8);   /* 链尾标记 */

    /* 覆盖时释放旧链（在新链分配成功之后，避免中途失败丢数据） */
    if (overwriting && old_first_cluster >= 2) fat_free_chain(old_first_cluster);

    /* 写数据扇区（最后一簇不足的部分补零） */
    const uint8_t* src = (const uint8_t*)data;
    uint32_t remaining = size;
    uint32_t c = first_cluster;
    while (remaining > 0 && c) {
        uint32_t lba = first_data_sector + (c - 2) * sectors_per_cluster;
        for (uint32_t s = 0; s < sectors_per_cluster && remaining > 0; s++) {
            memset(sector_buffer, 0, bytes_per_sector);
            uint32_t copy = (remaining < bytes_per_sector) ? remaining : bytes_per_sector;
            memcpy(sector_buffer, src, copy);
            if (!write_sector(lba + s, sector_buffer)) {
                /* ⚠️ 写扇区失败时释放已分配的簇链，避免磁盘空间永久泄漏 */
                fat_free_chain(first_cluster);
                return false;
            }
            src += copy;
            remaining -= copy;
        }
        uint32_t next = read_fat_entry(c);
        if (next >= 0x0FFFFFF8) break;
        c = next;
    }

    /* 填充目录项并写回 */
    if (!read_sector(entry_sector, sector_buffer)) return false;
    fat_dirent_t* e = (fat_dirent_t*)(sector_buffer + entry_offset);
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
    if (!write_sector(entry_sector, sector_buffer)) return false;

    return true;
}

/* ⚠️ 架构升级（内核态抢占）：锁包装 */
bool fat_write_file(const char* filename, const void* data, uint32_t size) {
    uint32_t fl = irq_lock();
    bool r = fat_write_file_impl(filename, data, size);
    irq_unlock(fl);
    return r;
}

/* 建子目录：分配一簇清零 → 写 '.'（指向自己）'..'（指向父）→
 * 父目录找空闲项（满则扩展）→ 填目录项（属性 0x10） */
static bool fat_mkdir_impl(const char* path) {
    if (!fat_initialized || !path) return false;

    char name_83[12];
    fat_dirent_t entry;
    bool exists = false;
    uint32_t parent = fat_resolve_parent(path, name_83, &entry, &exists);
    if (parent == 0xFFFFFFFF || exists) return false;

    uint32_t data_sectors = total_sectors - (bpb.reserved_sectors + bpb.num_fats * fat_size + root_dir_sectors);
    uint32_t max_cluster = 2 + data_sectors / sectors_per_cluster;

    /* 分配一个新簇存放目录内容 */
    uint32_t new_cluster = 0;
    for (uint32_t c = 2; c < max_cluster; c++) {
        if (read_fat_entry(c) == 0x0000) { new_cluster = c; break; }
    }
    if (new_cluster == 0) return false;   /* 磁盘满 */

    write_fat_entry(new_cluster, 0xFFF8);   /* 单簇目录，链尾 */
    uint32_t lba = first_data_sector + (new_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        memset(sector_buffer, 0, bytes_per_sector);
        write_sector(lba + s, sector_buffer);
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
    uint32_t parent_cluster = (parent == 0 && fs_type == FAT32) ? 0x0FFFFFF8 : parent;
    dotdot.cluster_low = (uint16_t)(parent_cluster & 0xFFFF);
    dotdot.cluster_high = (uint16_t)((parent_cluster >> 16) & 0xFFFF);

    /* ⚠️ 以下各失败点统一释放 new_cluster，避免泄漏 */
    if (!read_sector(lba, sector_buffer)) { fat_free_chain(new_cluster); return false; }
    memcpy(sector_buffer, &dot, sizeof(dot));
    memcpy(sector_buffer + 32, &dotdot, sizeof(dotdot));
    if (!write_sector(lba, sector_buffer)) { fat_free_chain(new_cluster); return false; }

    /* 在父目录写新目录项（满则扩展） */
    uint32_t entry_sector, entry_offset;
    int slot = dir_find_slot(parent, name_83, &entry_sector, &entry_offset);
    if (slot == 0) {
        if (parent == 0) { fat_free_chain(new_cluster); return false; }   /* 根目录固定区满 */
        uint32_t new_lba;
        if (!dir_extend(parent, &new_lba)) { fat_free_chain(new_cluster); return false; }
        entry_sector = new_lba;
        entry_offset = 0;
    } else if (slot == 1) {
        fat_free_chain(new_cluster);   /* 同名已存在 */
        return false;
    }

    if (!read_sector(entry_sector, sector_buffer)) { fat_free_chain(new_cluster); return false; }
    fat_dirent_t* e = (fat_dirent_t*)(sector_buffer + entry_offset);
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
    if (!write_sector(entry_sector, sector_buffer)) { fat_free_chain(new_cluster); return false; }

    return true;
}

/* ⚠️ 架构升级（内核态抢占）：锁包装 */
bool fat_mkdir(const char* path) {
    uint32_t fl = irq_lock();
    bool r = fat_mkdir_impl(path);
    irq_unlock(fl);
    return r;
}
