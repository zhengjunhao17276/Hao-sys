#ifndef BLOCK_DEV_H
#define BLOCK_DEV_H

#include <stdint.h>
#include <stdbool.h>

/* 块设备抽象：FAT 通过函数指针读写扇区，ATA 和 USB MSC 各实现一个后端 */
typedef struct {
    const char* name;                      /* 设备名，如 "ata0" / "usb0" */
    bool (*read_sector)(uint32_t lba, void* buf);    /* 返回 true=成功 */
    bool (*write_sector)(uint32_t lba, const void* buf);
    uint32_t sector_count;                 /* 总扇区数（0=未知） */
} block_dev_t;

#endif
