/**
 * =========================================================================
 * io.h - x86 I/O 端口操作内联函数
 *
 * x86 架构使用独立的 I/O 地址空间（与内存地址空间分开），通过 in/out
 * 指令族访问。访问 I/O 端口需要特权指令（ring 0 或 TSS 中 I/O 位图允许），
 * 所以这些函数只能在内核中使用。
 *
 * 提供的函数（按宽度）：
 *   inb/outb  - 8 位（字节）I/O 操作，端口范围 0x0000-0xFFFF
 *   inw/outw  - 16 位（字）I/O 操作
 *   inl/outl  - 32 位（双字）I/O 操作
 *
 * 使用场景举例：
 *   - VGA 控制寄存器通过端口 0x3D4/0x3D5 访问
 *   - PIC 通过 0x20/0x21（主片）和 0xA0/0xA1（从片）编程
 *   - ATA 硬盘通过 0x1F0-0x1F7 端口控制
 *   - PS/2 键盘鼠标通过 0x60/0x64 端口通信
 *   - UHCI USB 控制器通过 PCI 分配的 I/O 基址访问寄存器
 *
 * 所有函数声明为 static inline 以避免函数调用开销，因为 I/O 操作通常
 * 是时间敏感的（尤其在 ATA PIO 模式下）。
 * =========================================================================
 */

#ifndef IO_H
#define IO_H

#include <stdint.h>

/**
 * inb - 从指定 I/O 端口读取一个字节（8 位）
 * @port: I/O 端口号（0x0000 ~ 0xFFFF）
 * 返回值：从端口读到的 8 位值
 *
 * C 语言中嵌入 inb 指令：
 *   "=a"(ret)  → 将 AL 寄存器的值存入 ret
 *   "d"(port)  → 将 port 加载到 DX 寄存器（I/O 指令的端口号寄存器）
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

/**
 * outb - 向指定 I/O 端口写入一个字节（8 位）
 * @port: I/O 端口号
 * @val:  要写入的 8 位值
 *
 * "a"(val)  → 将 val 加载到 AL 寄存器（数据来源）
 * "d"(port) → 将 port 加载到 DX 寄存器（端口选择）
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "d"(port));
}

/**
 * inw - 从指定 I/O 端口读取一个字（16 位）
 * 用于读取 ATA 数据寄存器（ATA PIO 模式一次传送 16 位）
 */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

/**
 * outw - 向指定 I/O 端口写入一个字（16 位）
 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "d"(port));
}

/**
 * inl - 从指定 I/O 端口读取一个双字（32 位）
 * PCI 配置空间访问（0xCF8/0xCFC）使用双字读写
 */
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

/**
 * outl - 向指定 I/O 端口写入一个双字（32 位）
 */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "d"(port));
}

#endif
