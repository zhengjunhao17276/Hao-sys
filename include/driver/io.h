/*
 * io.h - x86 I/O 端口操作（in/out 指令族）
 * 特权指令仅内核可用；static inline 免调用开销。
 */

#ifndef IO_H
#define IO_H

#include <stdint.h>

/* 读端口 8 位 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

/* 写端口 8 位 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "d"(port));
}

/* 读端口 16 位（ATA PIO 用） */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

/* 写端口 16 位 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "d"(port));
}

/* 读端口 32 位（PCI 配置空间用） */
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

/* 写端口 32 位 */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "d"(port));
}

#endif
