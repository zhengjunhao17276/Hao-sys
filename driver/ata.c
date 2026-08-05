/**
 * =========================================================================
 * ata.c - ATA PIO 模式磁盘驱动
 *
 * ATA（Advanced Technology Attachment）驱动使用 PIO（Programmed I/O）
 * 模式——CPU 通过 in/out 指令直接读写磁盘数据寄存器，不经过 DMA。
 *
 * 28 位 LBA 寻址方式：
 *   扇区编号从 0 开始，最大 2^28-1（约 128GB）。
 *   LBA 地址通过 4 个 I/O 端口分开发送：
 *     0x1F3: LBA 位 0-7
 *     0x1F4: LBA 位 8-15
 *     0x1F5: LBA 位 16-23
 *     0x1F6: bit 0-3 = LBA 位 24-27, bit 4 = 主/从盘, bit 6 = LBA 模式
 *
 * 状态寄存器（0x1F7）的位定义：
 *   bit 7 (BSY): 设备忙，必须等待清零后才能发送命令
 *   bit 6 (DRDY): 设备就绪
 *   bit 3 (DRQ): 数据请求——设备准备好发送/接收数据
 *   bit 0 (ERR): 错误
 *
 * PIO 数据传输：
 *   每个扇区（512 字节）通过数据寄存器 0x1F0 以 16 位为单位传输，
 *   共需读写 256 次。
 * =========================================================================
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

/**
 * wait_bsy - 等待 ATA 设备 BSY 位清零
 *
 * 任何 ATA 命令操作前都需要确保设备不忙（BSY=0）。
 * 超时约 10 万次轮询（~1ms 硬件时间）。返回后同时检查 ERR 位。
 *
 * 返回：true = 设备就绪，false = 超时或错误
 */
static bool wait_bsy(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & 0x80)) {
            /* BSY 已清除 */
            if (st & 0x01) return false;  /* 错误位置位 */
            return true;                   /* 设备就绪 */
        }
    }
    return false;  /* 超时 */
}

/**
 * wait_drq - 等待 ATA 设备 DRQ（数据请求）就绪
 *
 * DRQ 置位表示设备已将数据放入缓冲区（读操作）或准备好接收数据
 * （写操作），此时可以开始数据传输。
 *
 * 返回：true = DRQ 就绪，false = 超时或错误
 */
static bool wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & 0x80) continue;           /* 还在忙，继续等待 */
        if (st & 0x08) return true;        /* DRQ 置位，可以传输 */
        if (st & 0x01) return false;       /* 错误 */
    }
    return false;  /* 超时 */
}

/**
 * wait_complete - 等待 ATA 操作完成（BSY 清除且无错误）
 *
 * 用于命令执行后的最后确认（如 IDENTIFY 命令的数据读取后）。
 * 返回：true = 成功完成，false = 超时或错误
 */
static bool wait_complete(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & 0x80)) {
            if (st & 0x01) return false;   /* 错误 */
            return true;                    /* 命令完成 */
        }
    }
    return false;  /* 超时 */
}

/**
 * ata_init - 初始化 ATA 主通道，探测设备存在性
 * @primary: 是否主通道（当前忽略，固定使用主通道 0x1F0）
 *
 * 发送 IDENTIFY 命令（0xEC）：如果设备存在，它会将 256 字（512 字节）
 * 的识别数据放入缓冲区，然后置位 DRQ。本驱动读取所有数据确认设备
 * 存在，但不解析识别数据的具体内容。
 *
 * 返回：true = 设备存在且响应正常
 */
bool ata_init(bool primary) {
    (void)primary;  /* 目前只使用主通道 */

    vga_write("[ATA] Initializing primary channel...\n");

    /* 第一步：等待设备不忙 */
    if (!wait_bsy()) {
        vga_write("[ATA] Timeout waiting for BSY clear.\n");
        ata_present = false;
        return false;
    }
    vga_write("[ATA] BSY cleared.\n");

    /* 设置命令参数（选择主盘、LBA 模式） */
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_DEVICE, 0xE0);      /* 0xE0 = bit 6(LBA) + bit 5(主盘) + 0 */
    outb(ATA_CMD, 0xEC);         /* 0xEC = IDENTIFY 命令 */

    /* 等待 DRQ 就绪——如果超时，说明无设备 */
    if (!wait_drq()) {
        vga_write("[ATA] No device detected (DRQ timeout).\n");
        ata_present = false;
        return false;
    }
    vga_write("[ATA] DRQ ready, reading data...\n");

    /* 读取 256 个字（512 字节）的 IDENTIFY 数据，确认设备存在 */
    for (int i = 0; i < 256; i++) inw(ATA_DATA);

    /* 最终确认无错误 */
    if (!wait_complete()) {
        vga_write("[ATA] Error after IDENTIFY.\n");
        ata_present = false;
        return false;
    }

    ata_present = true;
    vga_write("[ATA] Device detected (PIO mode).\n");
    return true;
}

/**
 * ata_read_sector - 使用 28 位 LBA 地址读取一个扇区
 * @lba:    扇区号（0 ~ 2^28-1）
 * @buffer: 512 字节缓冲区的指针
 *
 * 命令序列：
 *   1. 写入扇区计数
 *   2. 写入 LBA 地址（低 24 位→0x1F3/0x1F4/0x1F5，高 4 位→0x1F6）
 *   3. 写入命令 0x20（Read Sectors）
 *   4. 等待 DRQ，然后读取 256 次 × 16 位得到 512 字节
 *   5. 确认命令完成
 */
bool ata_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!ata_present) return false;
    if (!wait_bsy()) return false;

    /* 设置 LBA 参数 */
    outb(ATA_SECTOR_COUNT, 1);                    /* 读取 1 个扇区 */
    outb(ATA_LBA_LOW,   lba & 0xFF);              /* LBA 位 0-7 */
    outb(ATA_LBA_MID,  (lba >> 8) & 0xFF);        /* LBA 位 8-15 */
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);        /* LBA 位 16-23 */
    outb(ATA_DEVICE, 0xE0 | ((lba >> 24) & 0x0F)); /* bit4(主盘), bit6(LBA), 高位 */
    outb(ATA_CMD, 0x20);                           /* 0x20 = Read Sectors */

    if (!wait_drq()) return false;

    /* PIO 数据读取：256 次 × 16 位 = 512 字节 */
    for (int i = 0; i < 256; i++) {
        ((uint16_t*)buffer)[i] = inw(ATA_DATA);
    }

    /* 等待操作完成并检查错误 */
    if (!wait_complete()) return false;
    return true;
}

/**
 * ata_write_sector - 使用 28 位 LBA 地址写入一个扇区
 * @lba:    目标扇区号
 * @buffer: 要写入的 512 字节数据
 *
 * 命令序列：
 *   1. 写入扇区计数
 *   2. 写入 LBA 地址（同读操作）
 *   3. 写入命令 0x30（Write Sectors）
 *   4. 等待 DRQ，然后写入 256 次 × 16 位
 *   5. 等待写入完成
 */
bool ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!ata_present) return false;
    if (!wait_bsy()) return false;

    /* 设置 LBA 参数 */
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW,   lba & 0xFF);
    outb(ATA_LBA_MID,  (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_DEVICE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_CMD, 0x30);                          /* 0x30 = Write Sectors */

    if (!wait_drq()) return false;

    /* PIO 数据写入：256 次 × 16 位 = 512 字节 */
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, ((uint16_t*)buffer)[i]);
    }

    /* 等待写入完成并检查错误（写入比读取更需要注意完成状态） */
    if (!wait_complete()) return false;
    return true;
}
