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

/* 初始化并探测设备（IDENTIFY 0xEC），存在且响应正常返回 true */
bool ata_init(bool primary);

/* 磁盘总扇区数（IDENTIFY words 60-61，LBA28）；0=未知 */
extern uint32_t ata_sector_count;

/* 读一个扇区（512 字节）到 buffer */
bool ata_read_sector(uint32_t lba, uint8_t *buffer);

/* 写一个扇区（512 字节） */
bool ata_write_sector(uint32_t lba, const uint8_t *buffer);

#endif
