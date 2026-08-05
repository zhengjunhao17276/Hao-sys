/**
 * =========================================================================
 * pci.c - PCI 总线枚举与设备扫描
 *
 * PCI 配置空间访问机制：
 *   通过两个 32 位 I/O 端口操作（CONFIG_ADDR + CONFIG_DATA）。
 *   写地址端口（0xCF8）指定目标，读写数据端口（0xCFC）传输数据。
 *
 *   地址格式（32 位）：
 *     bit 31:    使能位（必须为 1）
 *     bit 30-24: 保留
 *     bit 23-16: 总线号（0~255）
 *     bit 15-11: 设备号（0~31）
 *     bit 10-8:  功能号（0~7）
 *     bit 7-2:   寄存器偏移（自动 4 字节对齐）
 *
 * 枚举策略：
 *   从总线 0 开始，遍历所有设备/功能。对每个 PCI-PCI 桥接器
 *   （Class=0x06, Subclass=0x04），递归扫描下游总线。
 *   通过 scanned_buses[] 数组防止重复扫描。
 *
 * 多功能设备判断：
 *   读取 Header Type 寄存器的 bit 7——如果置位，说明该设备支持
 *   多个功能，需要扫描 func 0~7；否则只需扫描 func 0。
 * =========================================================================
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

/* 前向声明 */
static void pci_scan_bus(int bus);

/**
 * pci_read_config - 读取 PCI 配置空间寄存器的值
 *
 * 构造地址：使能位(31) + 总线(23:16) + 设备(15:11) + 功能(10:8) + 偏移(7:2)
 * offset 的低 2 位被忽略（自动对齐到 4 字节边界）。
 */
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (0x80000000)                     /* bit 31 = 1：使能 */
                  | ((uint32_t)bus << 16)             /* bit 23-16：总线号 */
                  | ((uint32_t)slot << 11)            /* bit 15-11：设备号 */
                  | ((uint32_t)func << 8)             /* bit 10-8：功能号 */
                  | (offset & 0xFC);                  /* bit 7-2：寄存器偏移 */
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

/**
 * pci_write_config - 向 PCI 配置空间寄存器写入值
 *
 * 用于配置设备资源（BAR 基址、中断线等）。
 * 注意：写入配置空间需要先写地址端口再写数据端口。
 */
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (0x80000000)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/**
 * scan_function - 扫描 PCI 设备的一个功能
 *
 * 读取 Vendor ID ——如果为 0xFFFF，说明该功能/设备不存在，直接返回。
 * 打印设备信息后，检查是否为 PCI-PCI 桥接器（Class=0x06, Subclass=0x04），
 * 如果是则读取其次总线号（Secondary Bus Number），递归扫描下游总线。
 *
 * PCI-PCI 桥接器的配置空间偏移 0x18：
 *   bit 0-7:   主总线号（Primary Bus）
 *   bit 8-15:  次总线号（Secondary Bus）← 下游总线的编号
 *   bit 16-23: 下级总线号（Subordinate Bus）← 下游最深的总线号
 */
static void scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    /* 读取 Vendor ID（寄存器 0x00 的低 16 位）——设备存在性的唯一判断标准 */
    uint32_t vid = pci_read_config(bus, slot, func, 0) & 0xFFFF;
    if (vid == 0xFFFF) return;  /* 0xFFFF = 该位置无设备 */

    /* 读取 Device ID 和类别码 */
    uint32_t dev = (pci_read_config(bus, slot, func, 0) >> 16) & 0xFFFF;
    uint32_t class_rev = pci_read_config(bus, slot, func, 0x08);
    uint8_t class = (class_rev >> 24) & 0xFF;
    uint8_t subclass = (class_rev >> 16) & 0xFF;

    /* 打印设备信息到 VGA 终端 */
    vga_write("  Bus ");
    vga_write_hex(bus);
    vga_write(" Slot ");
    vga_write_hex(slot);
    vga_write(" Func ");
    vga_write_hex(func);
    vga_write(": VID ");
    vga_write_hex(vid);
    vga_write(" DEV ");
    vga_write_hex(dev);
    vga_write(" Class ");
    vga_write_hex(class);
    vga_write(" Sub ");
    vga_write_hex(subclass);
    vga_write("\n");

    /* 检测 PCI-PCI 桥接器（Class=0x06, Subclass=0x04），递归扫描下游 */
    if (class == 0x06 && subclass == 0x04) {
        /* 读取桥接器的次总线号（寄存器 0x18，bit 8-15） */
        uint32_t bridge_reg = pci_read_config(bus, slot, func, 0x18);
        uint8_t secondary_bus = (bridge_reg >> 8) & 0xFF;
        uint8_t subordinate_bus = (bridge_reg >> 16) & 0xFF;

        vga_write("    -> PCI-PCI bridge to bus ");
        vga_write_hex(secondary_bus);
        vga_write(" (subordinate ");
        vga_write_hex(subordinate_bus);
        vga_write(")\n");

        /* 安全检查：次总线号不能等于当前总线，不能超过 255，且未扫描过 */
        if (secondary_bus != bus && !scanned_buses[secondary_bus]) {
            scanned_buses[secondary_bus] = true;
            pci_scan_bus(secondary_bus);
        } else {
            vga_write("    -> Skipping (invalid or already scanned)\n");
        }
    }
}

/**
 * pci_scan_bus - 扫描一条 PCI 总线上的所有设备
 *
 * 遍历 32 个设备槽位（Slot 0~31），对每个槽位先读 func 0 的 Vendor ID。
 * 如果 Vendor ID ≠ 0xFFFF，读取 Header Type 判断是否多功能设备：
 *   - bit 7 = 0：单功能设备 → 只扫描 func 0
 *   - bit 7 = 1：多功能设备 → 扫描 func 0~7
 */
static void pci_scan_bus(int bus) {
    if (bus < 0 || bus >= 256) return;  /* 总线号越界保护 */
    if (scanned_buses[bus]) return;   /* 已扫描，跳过 */
    scanned_buses[bus] = true;

    vga_write("[PCI] Scanning bus ");
    vga_write_hex(bus);
    vga_write("...\n");

    for (int slot = 0; slot < 32; slot++) {
        /* 读取 func 0 的 Vendor ID */
        uint32_t vid = pci_read_config(bus, slot, 0, 0) & 0xFFFF;
        if (vid == 0xFFFF) continue;  /* 空槽位，跳过 */

        /* 直接扫描全部 8 个功能（0xFFFF 会被 scan_function 自动跳过）。
         * 不依赖 Header Type 的多功能位判断——曾经因该判断失效
         * 漏掉了 PIIX3 的 USB 控制器（slot1 func2），导致 UHCI 永远
         * 发现不了。无条件扫描最简单可靠。 */
        for (int func = 0; func < 8; func++) {
            scan_function(bus, slot, func);
        }
    }
}

/**
 * pci_init - 执行完整的 PCI 总线枚举
 *
 * 从总线 0 开始递归扫描，自动发现 PCI-PCI 桥的级联拓扑。
 * 每次扫描前重置 scanned_buses[] 数组。
 */
void pci_init(void) {
    /* 重置扫描标记数组 */
    for (int i = 0; i <= 255; i++) scanned_buses[i] = false;

    vga_write("[PCI] Starting enumeration...\n");
    pci_scan_bus(0);
    vga_write("[PCI] Enumeration complete.\n");
}

/**
 * pci_scan - 对外扫描接口（别名）
 * 当前实现直接调用 pci_init()。
 */
void pci_scan(void) {
    pci_init();
}
