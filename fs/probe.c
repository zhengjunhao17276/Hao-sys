/*
 * probe.c - 文件系统识别实现
 *
 * 签名依据：
 *   FAT12/16/32  BPB 偏移 54-65 的 "FAT12   "/"FAT16   "/"FAT32   "；
 *               无字符串时按 BPB 数值回退（总簇数 <4085=FAT12，<65525=FAT16）
 *   NTFS        偏移 3-10 的 "NTFS    "
 *   exFAT       偏移 3-10 的 "EXFAT   "
 *   ext2/3/4    扇区 2 偏移 56 的魔数 0xEF53；incompat 有 extents(0x40)→ext4，
 *               否则 compat 有 journal(0x4)→ext3，否则 ext2
 *   ISO9660     扇区 16 偏移 1 的 "CD001"
 *   XFS         扇区 0 偏移 0 的 "XFSB"
 *   btrfs       扇区 128 偏移 0x40 的 "_BHRfS_M"
 */

#include "../include/fs/probe.h"
#include "../include/lib/string.h"

const char* probe_fs_name(probe_fs_t type) {
    switch (type) {
        case FS_FAT12:   return "FAT12";
        case FS_FAT16:   return "FAT16";
        case FS_FAT32:   return "FAT32";
        case FS_EXFAT:   return "exFAT";
        case FS_NTFS:    return "NTFS";
        case FS_EXT2:    return "ext2";
        case FS_EXT3:    return "ext3";
        case FS_EXT4:    return "ext4";
        case FS_ISO9660: return "ISO9660";
        case FS_XFS:     return "XFS";
        case FS_BTRFS:   return "btrfs";
        default:         return "unknown";
    }
}

/* FAT 数值回退检测（镜像 fat.c 的判定：总簇数阈值） */
static probe_fs_t probe_fat_numeric(const uint8_t* s) {
    uint16_t bps = (uint16_t)s[11] | ((uint16_t)s[12] << 8);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return FS_UNKNOWN;
    uint8_t spc = s[13];
    if (spc == 0 || (spc & (spc - 1)) != 0) return FS_UNKNOWN;   /* 2 的幂 */
    uint16_t reserved = (uint16_t)s[14] | ((uint16_t)s[15] << 8);
    uint8_t nfats = s[16];
    if (nfats == 0) return FS_UNKNOWN;
    uint16_t root_entries = (uint16_t)s[17] | ((uint16_t)s[18] << 8);
    uint16_t tot16 = (uint16_t)s[19] | ((uint16_t)s[20] << 8);
    uint32_t tot32 = (uint32_t)s[32] | ((uint32_t)s[33] << 8) |
                     ((uint32_t)s[34] << 16) | ((uint32_t)s[35] << 24);
    uint16_t fatsz16 = (uint16_t)s[22] | ((uint16_t)s[23] << 8);
    uint32_t fatsz = fatsz16;
    if (fatsz == 0)
        fatsz = (uint32_t)s[36] | ((uint32_t)s[37] << 8) |
                ((uint32_t)s[38] << 16) | ((uint32_t)s[39] << 24);
    if (fatsz == 0) return FS_UNKNOWN;
    uint32_t total = tot16 ? tot16 : tot32;
    if (total == 0) return FS_UNKNOWN;

    uint32_t root_sectors = (root_entries * 32 + bps - 1) / bps;
    uint32_t data_sectors = total - reserved - nfats * fatsz - root_sectors;
    uint32_t clusters = data_sectors / spc;

    if (clusters < 4085) return FS_FAT12;
    if (clusters < 65525) return FS_FAT16;
    return FS_FAT32;
}

probe_fs_t fs_probe(const block_dev_t* dev) {
    if (!dev || !dev->read_sector) return FS_UNKNOWN;

    uint8_t s0[512], s2[512], s16[512], s128[512];
    bool have0 = dev->read_sector(0, s0);

    /* 扇区 0 签名 */
    if (have0) {
        if (memcmp(s0 + 3, "NTFS    ", 8) == 0) return FS_NTFS;
        if (memcmp(s0 + 3, "EXFAT   ", 8) == 0) return FS_EXFAT;
        if (memcmp(s0 + 54, "FAT12   ", 8) == 0) return FS_FAT12;
        if (memcmp(s0 + 54, "FAT16   ", 8) == 0) return FS_FAT16;
        if (memcmp(s0 + 54, "FAT32   ", 8) == 0) return FS_FAT32;
        if (memcmp(s0, "XFSB", 4) == 0) return FS_XFS;
        /* FAT 数值回退（无字符串但 BPB 合法） */
        probe_fs_t t = probe_fat_numeric(s0);
        if (t != FS_UNKNOWN) return t;
    }

    /* ext2/3/4：超级块在扇区 2 偏移 56（魔数 0xEF53） */
    if (dev->read_sector(2, s2)) {
        uint16_t magic = (uint16_t)s2[56] | ((uint16_t)s2[57] << 8);
        if (magic == 0xEF53) {
            uint32_t incompat = (uint32_t)s2[96] | ((uint32_t)s2[97] << 8) |
                                ((uint32_t)s2[98] << 16) | ((uint32_t)s2[99] << 24);
            uint32_t compat = (uint32_t)s2[92] | ((uint32_t)s2[93] << 8) |
                              ((uint32_t)s2[94] << 16) | ((uint32_t)s2[95] << 24);
            if (incompat & 0x40) return FS_EXT4;      /* EXT4_FEATURE_INCOMPAT_EXTENTS */
            if (compat & 0x04) return FS_EXT3;        /* EXT3_FEATURE_COMPAT_HAS_JOURNAL */
            return FS_EXT2;
        }
    }

    /* ISO9660：卷描述符在扇区 16，偏移 1 = "CD001" */
    if (dev->read_sector(16, s16)) {
        if (s16[1] == 'C' && s16[2] == 'D' && s16[3] == '0' &&
            s16[4] == '0' && s16[5] == '1') return FS_ISO9660;
    }

    /* btrfs：超级块魔数在扇区 128 偏移 0x40 */
    if (dev->read_sector(128, s128)) {
        if (memcmp(s128 + 0x40, "_BHRfS_M", 8) == 0) return FS_BTRFS;
    }

    return FS_UNKNOWN;
}
