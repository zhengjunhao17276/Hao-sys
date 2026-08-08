/*
 * ata.h - ATA PIO 模式磁盘驱动接口（主通道 0x1F0，28 位 LBA）
 */

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

/* ATA 通道 I/O 基址 */
#define ATA_PRIMARY_IO   0x1F0   /* 主通道基址 */
#define ATA_SECONDARY_IO 0x170   /* 次通道基址（本驱动当前只使用主通道） */

/* 盘位选择位（写 ATA_DEVICE 寄存器：bit6=LBA + bit5=主/从） */
#define ATA_DRIVE_MASTER 0xE0
#define ATA_DRIVE_SLAVE  0xF0

/* 初始化并探测设备（IDENTIFY 0xEC），主盘存在且响应正常返回 true */
bool ata_init(bool primary);

/* 磁盘总扇区数（IDENTIFY words 60-61，LBA28）；0=未知 */
extern uint32_t ata_sector_count;

/* 从盘（同通道 slave）状态 */
extern uint32_t ata_slave_sector_count;

/* 读一个扇区（512 字节）到 buffer（主盘） */
bool ata_read_sector(uint32_t lba, uint8_t *buffer);

/* 写一个扇区（512 字节）（主盘） */
bool ata_write_sector(uint32_t lba, const uint8_t *buffer);

/* 从盘读写（第二块盘/文件系统识别测试盘） */
bool ata_read_sector_slave(uint32_t lba, uint8_t *buffer);
bool ata_write_sector_slave(uint32_t lba, const uint8_t *buffer);

#endif
