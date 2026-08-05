/**
 * =========================================================================
 * ata.h - ATA PIO 模式驱动接口
 *
 * 硬件背景：
 *   ATA（Advanced Technology Attachment）是传统的 IDE 硬盘接口标准。
 *   本驱动使用 PIO（Programmed I/O）模式——CPU 通过 I/O 端口逐字读写
 *   数据，不从 DMA 控制器。
 *
 *   端口映射（主通道，I/O 基址 0x1F0）：
 *     0x1F0: 数据寄存器（16 位，读写数据在此）
 *     0x1F2: 扇区计数
 *     0x1F3-0x1F5: LBA 地址低/中/高字节
 *     0x1F6: 驱动/磁头选择（含 LBA 高位）
 *     0x1F7: 命令/状态寄存器
 *
 * 28 位 LBA 寻址：
 *   支持最大 128GB 的磁盘（2^28 × 512 字节），大多数旧硬盘和
 *   QEMU 模拟的磁盘都在此范围内。
 * =========================================================================
 */

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

/* ATA 通道 I/O 基址 */
#define ATA_PRIMARY_IO   0x1F0   /* 主通道基址 */
#define ATA_SECONDARY_IO 0x170   /* 次通道基址（本驱动当前只使用主通道） */

/**
 * ata_init - 初始化 ATA 主通道
 * @primary: 是否使用主通道（目前忽略，固定使用主通道）
 *
 * 发送 IDENTIFY 命令，等待设备响应。如果收到有效的 DRQ 响应，
 * 读取 256 个字的识别数据（本驱动目前不解析这些数据，仅确认设备存在）。
 * 返回 true 表示设备存在且响应正常。
 */
bool ata_init(bool primary);

/**
 * ata_read_sector - 从磁盘读取一个扇区（512 字节）
 * @lba:    28 位 LBA 扇区号（0 ~ 2^28-1）
 * @buffer: 用于存放读取数据的缓冲区（至少 512 字节）
 * 返回：成功返回 true，失败（超时/错误）返回 false
 */
bool ata_read_sector(uint32_t lba, uint8_t *buffer);

/**
 * ata_write_sector - 向磁盘写入一个扇区（512 字节）
 * @lba:    28 位 LBA 扇区号
 * @buffer: 要写入的 512 字节数据
 * 返回：成功返回 true，失败返回 false
 */
bool ata_write_sector(uint32_t lba, const uint8_t *buffer);

#endif
