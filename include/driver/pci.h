/**
 * =========================================================================
 * pci.h - PCI 总线扫描与配置空间访问接口
 *
 * 硬件背景：
 *   PCI 配置空间通过两个 32 位 I/O 端口访问：
 *     - 0xCF8: 配置地址端口（CONFIG_ADDRESS），指定要访问的
 *             总线/设备/功能号和寄存器偏移
 *     - 0xCFC: 配置数据端口（CONFIG_DATA），读写该地址对应的
 *             配置空间寄存器值
 *
 * 枚举过程：
 *   从总线 0 开始扫描，对每个设备/功能读取 Vendor ID（0xFFFF = 空槽），
 *   遇到 PCI-PCI 桥接器（Class=0x06, Subclass=0x04）时递归扫描下游总线。
 *   这样就覆盖了整个 PCI 层级结构。
 *
 * 本扫描不区分多功能设备——对于 Header Type 的 bit 7 置位的设备，
 * 会扫描全部 8 个功能。
 * =========================================================================
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/**
 * pci_device_t - PCI 设备信息结构体
 * 由扫描过程填充，供调用者检查发现的设备。
 */
typedef struct {
    uint16_t vendor_id;    /* 厂商 ID（如 Intel=0x8086，QEMU=0x1234） */
    uint16_t device_id;    /* 设备 ID（由厂商定义） */
    uint8_t  bus;          /* 设备所在总线号 */
    uint8_t  slot;         /* 设备所在插槽号 */
    uint8_t  func;         /* 功能号 */
    uint8_t  class_code;   /* 类别码（如 0x0C=USB, 0x01=ATA, 0x06=桥） */
    uint8_t  subclass;     /* 子类别码（如 0x03=USB 控制器, 0x04=PCI-PCI 桥） */
} pci_device_t;

/**
 * pci_init - 执行完整的 PCI 总线枚举
 * 从总线 0 开始递归扫描，输出发现的设备信息到 VGA 终端。
 */
void pci_init(void);

/**
 * pci_scan - 扫描 PCI 总线的别名函数
 * 目前实现为直接调用 pci_init()。
 */
void pci_scan(void);

/**
 * pci_read_config - 读取 PCI 配置空间寄存器的值
 * @bus:    总线号 (0~255)
 * @slot:   设备号 (0~31)
 * @func:   功能号 (0~7)
 * @offset: 寄存器偏移地址（必须是 4 字节对齐）
 * 返回值：配置空间寄存器的 32 位值
 */
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/**
 * pci_write_config - 向 PCI 配置空间寄存器写入值
 * @bus:    总线号
 * @slot:   设备号
 * @func:   功能号
 * @offset: 寄存器偏移地址（必须是 4 字节对齐）
 * @val:    要写入的 32 位值
 *
 * 用于修改设备的 BAR、中断线、电源管理等配置。
 */
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

#endif
