/*
 * pci.h - PCI 总线扫描与配置空间访问接口（0xCF8/0xCFC）
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* PCI 设备信息（扫描时填充） */
typedef struct {
    uint16_t vendor_id;    /* 厂商 ID（如 Intel=0x8086，QEMU=0x1234） */
    uint16_t device_id;    /* 设备 ID（由厂商定义） */
    uint8_t  bus;          /* 设备所在总线号 */
    uint8_t  slot;         /* 设备所在插槽号 */
    uint8_t  func;         /* 功能号 */
    uint8_t  class_code;   /* 类别码（如 0x0C=USB, 0x01=ATA, 0x06=桥） */
    uint8_t  subclass;     /* 子类别码（如 0x03=USB 控制器, 0x04=PCI-PCI 桥） */
} pci_device_t;

/* 从总线 0 递归枚举整个 PCI 拓扑 */
void pci_init(void);

/* pci_init 的别名 */
void pci_scan(void);

/* 读配置空间寄存器（offset 需 4 字节对齐） */
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* 写配置空间寄存器（BAR、中断线等） */
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

#endif
