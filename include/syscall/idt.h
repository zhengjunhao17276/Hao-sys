/*
 * idt.h - IDT 接口
 * 向量布局：0x00-0x13 CPU 异常（DPL=0），0x20-0x2F 硬件 IRQ（PIC 转发），
 * 0x80 系统调用（DPL=3）。门类型：中断门自动 cli、陷阱门不关、任务门少用。
 */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/*
 * idt_entry_t - IDT 条目（8 字节）
 * handler = (base_high << 16) | base_low；selector 内核用 0x08。
 * flags: bit0-3=type(0xE 中断门), bit5-6=DPL, bit7=P。
 */
typedef struct {
    uint16_t base_low;      /* 处理程序地址低 16 位 */
    uint16_t selector;      /* 代码段选择子（内核用 0x08） */
    uint8_t  zero;          /* 保留，必须为 0 */
    uint8_t  flags;         /* 类型与属性（P=0x80, DPL=0x60, type=0x0E 中断门） */
    uint16_t base_high;     /* 处理程序地址高 16 位 */
} __attribute__((packed)) idt_entry_t;

/* 供 lidt 用：limit = IDT 总字节数 - 1，base = IDT 线性地址 */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idtr_t;

/* 初始化全部 256 个门并 lidt */
void idt_init(void);

/* 设置一个门：flags bit7=P, bit5-6=DPL, bit0-3=type */
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags);

#endif
