/*
 * pci.c - PCI 总线枚举与配置空间访问
 * 0xCF8：bit31 使能 | 总线(23:16) | 设备(15:11) | 功能(10:8) | 偏移(7:2)；
 * 从总线 0 递归扫描，遇 PCI-PCI 桥（06/04）下沉，scanned_buses 防重复。
 */

#include "../include/driver/io.h"
#include "../include/driver/pci.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>

/* PCI 配置空间访问端口 */
#define PCI_CONFIG_ADDR 0xCF8   /* 配置地址端口（CONFIG_ADDRESS） */
#define PCI_CONFIG_DATA 0xCFC   /* 配置数据端口（CONFIG_DATA） */

/* 已扫描总线标记（最多 256 条总线） */
static bool scanned_buses[256];
static uint32_t pci_device_count = 0;   /* 枚举到的设备数（保留计数，供将来使用） */

/* 前向声明 */
static void pci_scan_bus(int bus);

/* 读配置空间：offset 自动 4 字节对齐（低 2 位忽略） */
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (0x80000000)                     /* 使能位 */
                  | ((uint32_t)bus << 16)             /* 总线号 */
                  | ((uint32_t)slot << 11)            /* 设备号 */
                  | ((uint32_t)func << 8)             /* 功能号 */
                  | (offset & 0xFC);                  /* 寄存器偏移 */
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

/* 写配置空间（BAR、中断线等）：先写地址端口再写数据端口 */
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (0x80000000)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* 扫一个功能：VID=0xFFFF 即空位；桥设备（06/04）读 0x18 次总线号递归下沉 */
static void scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vid = pci_read_config(bus, slot, func, 0) & 0xFFFF;   /* VID = 存在性判据 */
    if (vid == 0xFFFF) return;
    pci_device_count++;

    uint32_t class_rev = pci_read_config(bus, slot, func, 0x08);
    uint8_t class = (class_rev >> 24) & 0xFF;
    uint8_t subclass = (class_rev >> 16) & 0xFF;

    if (class == 0x06 && subclass == 0x04) {
        /* 桥：读 0x18 的次总线号（bit 8-15） */
        uint32_t bridge_reg = pci_read_config(bus, slot, func, 0x18);
        uint8_t secondary_bus = (bridge_reg >> 8) & 0xFF;

        /* 防环：次总线号不能等于当前总线，且不能重复扫描 */
        if (secondary_bus != bus && !scanned_buses[secondary_bus]) {
            scanned_buses[secondary_bus] = true;
            pci_scan_bus(secondary_bus);
        }
    }
}

/* 扫一条总线：32 个槽位 × 8 个功能 */
static void pci_scan_bus(int bus) {
    if (bus < 0 || bus >= 256) return;
    if (scanned_buses[bus]) return;
    scanned_buses[bus] = true;

    for (int slot = 0; slot < 32; slot++) {
        uint32_t vid = pci_read_config(bus, slot, 0, 0) & 0xFFFF;
        if (vid == 0xFFFF) continue;

        /* 直接扫描全部 8 个功能（0xFFFF 会被 scan_function 自动跳过）。
         * 不依赖 Header Type 的多功能位判断——曾经因该判断失效
         * 漏掉了 PIIX3 的 USB 控制器（slot1 func2），导致 UHCI 永远
         * 发现不了。无条件扫描最简单可靠。 */
        for (int func = 0; func < 8; func++) {
            scan_function(bus, slot, func);
        }
    }
}

/* 从总线 0 递归枚举整个 PCI 拓扑 */
void pci_init(void) {
    for (int i = 0; i <= 255; i++) scanned_buses[i] = false;
    pci_device_count = 0;

    pci_scan_bus(0);
}

/* pci_init 的别名 */
void pci_scan(void) {
    pci_init();
}
