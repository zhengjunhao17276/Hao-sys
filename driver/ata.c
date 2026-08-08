/*
 * ata.c - ATA PIO 模式磁盘驱动（主通道 0x1F0，28 位 LBA）
 * 状态寄存器 0x1F7：bit7=BSY、bit6=DRDY、bit3=DRQ、bit0=ERR；
 * 数据以 16 位为单位读写 0x1F0，每扇区 256 次。
 */

#include "../include/driver/io.h"
#include "../include/driver/ata.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- ATA 寄存器端口定义（主通道） ---- */
#define ATA_DATA         0x1F0  /* 数据寄存器（16 位，PIO 数据传输） */
#define ATA_FEATURES     0x1F1  /* 特性寄存器 */
#define ATA_SECTOR_COUNT 0x1F2  /* 扇区计数（要操作的扇区数） */
#define ATA_LBA_LOW      0x1F3  /* LBA 地址位 0-7 */
#define ATA_LBA_MID      0x1F4  /* LBA 地址位 8-15 */
#define ATA_LBA_HIGH     0x1F5  /* LBA 地址位 16-23 */
#define ATA_DEVICE       0x1F6  /* 驱动器/磁头选择 + LBA 位 24-27 */
#define ATA_CMD          0x1F7  /* 命令寄存器（写） */
#define ATA_STATUS       0x1F7  /* 状态寄存器（读），与命令端口同一地址 */

/* ATA 是否存在的标志（由 ata_init 设置） */
static bool ata_present = false;

/* 磁盘总扇区数（IDENTIFY words 60-61，LBA28）；0=未知 */
uint32_t ata_sector_count = 0;

/* 从盘（同一主通道的 slave）状态 */
static bool ata_slave_present = false;
uint32_t ata_slave_sector_count = 0;

/* 等 BSY 清零（约 10 万次轮询），顺带检查 ERR 位 */
static bool wait_bsy(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & 0x80)) {
            if (st & 0x01) return false;
            return true;
        }
    }
    return false;
}

/* 等 DRQ 就绪（设备备好数据 / 可接收数据） */
static bool wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & 0x80) continue;
        if (st & 0x08) return true;
        if (st & 0x01) return false;
    }
    return false;
}

/* 等命令完成（BSY 清且无 ERR），命令收尾确认用 */
static bool wait_complete(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & 0x80)) {
            if (st & 0x01) return false;
            return true;
        }
    }
    return false;
}

/* 探测设备：发 IDENTIFY（0xEC），读到 DRQ、清完 256 字数据即认为存在 */
/* 探测指定盘位：发 IDENTIFY（0xEC），读到 DRQ、清完 256 字数据即认为存在。
 * drive_select：0xE0=主盘，0xF0=从盘（同一通道共用端口） */
static bool ata_identify(uint8_t drive_select, uint32_t* out_sectors) {
    if (!wait_bsy()) return false;

    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_DEVICE, drive_select);      /* bit6=1(LBA) + bit5(主/从) + 高 4 位 */
    outb(ATA_CMD, 0xEC);                 /* 0xEC = IDENTIFY 命令 */

    /* DRQ 超时即无设备 */
    if (!wait_drq()) return false;

    /* IDENTIFY 数据 256 字：word 60-61 = LBA28 总扇区数（低位在前） */
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    uint32_t sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (sectors == 0) sectors = 0x0FFFFFFF;   /* 未知则按 LBA28 上限 */
    *out_sectors = sectors;

    return wait_complete();
}

bool ata_init(bool primary) {
    (void)primary;  /* 目前只使用主通道 */

    vga_write("[ATA] Initializing primary channel...\n");

    /* 主盘 */
    ata_present = ata_identify(ATA_DRIVE_MASTER, &ata_sector_count);
    if (ata_present) {
        vga_write("[ATA] Master: sectors=");
        vga_write_hex(ata_sector_count);
        vga_write(" (PIO mode)\n");
    } else {
        vga_write("[ATA] No master device (DRQ timeout).\n");
    }

    /* 从盘（挂第二块盘/非 FAT 测试盘时用） */
    ata_slave_present = ata_identify(ATA_DRIVE_SLAVE, &ata_slave_sector_count);
    if (ata_slave_present) {
        vga_write("[ATA] Slave: sectors=");
        vga_write_hex(ata_slave_sector_count);
        vga_write(" (PIO mode)\n");
    }

    return ata_present;
}

/* 28 位 LBA 读扇区：写参数 → 0x20 → 等 DRQ → 256×16 位读数据 */
static bool ata_read_sector_dev(uint8_t drive_select, uint32_t lba, uint8_t* buffer) {
    if (!wait_bsy()) return false;

    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW,   lba & 0xFF);              /* LBA 位 0-7 */
    outb(ATA_LBA_MID,  (lba >> 8) & 0xFF);        /* LBA 位 8-15 */
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);        /* LBA 位 16-23 */
    outb(ATA_DEVICE, drive_select | ((lba >> 24) & 0x0F)); /* 主/从 + LBA 高 4 位 */
    outb(ATA_CMD, 0x20);                           /* Read Sectors */

    if (!wait_drq()) return false;

    /* PIO 读：256 次 × 16 位 = 512 字节 */
    for (int i = 0; i < 256; i++) {
        ((uint16_t*)buffer)[i] = inw(ATA_DATA);
    }

    if (!wait_complete()) return false;
    return true;
}

/* 主盘读（兼容旧 API） */
bool ata_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!ata_present) return false;
    return ata_read_sector_dev(ATA_DRIVE_MASTER, lba, buffer);
}

/* 从盘读 */
bool ata_read_sector_slave(uint32_t lba, uint8_t *buffer) {
    if (!ata_slave_present) return false;
    return ata_read_sector_dev(ATA_DRIVE_SLAVE, lba, buffer);
}

/* 28 位 LBA 写扇区：写参数 → 0x30 → 等 DRQ → 256×16 位写数据 */
static bool ata_write_sector_dev(uint8_t drive_select, uint32_t lba, const uint8_t* buffer) {
    if (!wait_bsy()) return false;

    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW,   lba & 0xFF);
    outb(ATA_LBA_MID,  (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_DEVICE, drive_select | ((lba >> 24) & 0x0F));
    outb(ATA_CMD, 0x30);                          /* Write Sectors */

    if (!wait_drq()) return false;

    /* PIO 写：256 次 × 16 位 = 512 字节 */
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, ((uint16_t*)buffer)[i]);
    }

    /* 等写入完成——写操作必须确认落盘，不能省 */
    if (!wait_complete()) return false;
    return true;
}

/* 主盘写（兼容旧 API） */
bool ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!ata_present) return false;
    return ata_write_sector_dev(ATA_DRIVE_MASTER, lba, buffer);
}

/* 从盘写 */
bool ata_write_sector_slave(uint32_t lba, const uint8_t *buffer) {
    if (!ata_slave_present) return false;
    return ata_write_sector_dev(ATA_DRIVE_SLAVE, lba, buffer);
}
