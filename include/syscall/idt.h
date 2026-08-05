/**
 * =========================================================================
 * idt.h - 中断描述符表（IDT）接口
 *
 * IDT 是 i386 架构的核心数据结构之一，告诉 CPU 每个中断向量（0~255）
 * 对应的处理程序在哪里。IDT 包含三种门：
 *   - 中断门（Interrupt Gate）：进入处理程序时自动关中断（CLI）
 *   - 陷阱门（Trap Gate）：不自动关中断
 *   - 任务门：通过硬件任务切换机制（很少使用）
 *
 * IDT 条目格式（8 字节）：
 *   位 0-15:  处理程序偏移低 16 位
 *   位 16-31: 代码段选择子
 *   位 32-39: 保留（必须为 0）
 *   位 40-47: 标志（存在位、DPL、门类型）
 *   位 48-63: 处理程序偏移高 16 位
 *
 * HaoOS 使用的中断向量布局：
 *   0x00-0x13: CPU 异常（Exception，由 CPU 自动触发）
 *   0x20-0x2F: 硬件 IRQ（由 PIC 转发）
 *   0x80:      系统调用（通过 int 0x80 软中断）
 * =========================================================================
 */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/**
 * idt_entry_t - IDT 条目结构体（8 字节）
 *
 * handler（处理程序地址）由 base_low 和 base_high 拼接而成：
 *   handler = (base_high << 16) | base_low
 *
 * selector 是 GDT 中的代码段选择子，内核中断处理用 0x08（内核代码段）。
 *
 * flags 位布局：
 *   bit 0-3:  门类型（0xE=中断门, 0xF=陷阱门）
 *   bit 4:     保留
 *   bit 5-6:   DPL（描述符特权级，0=内核, 3=用户）
 *   bit 7:     Present（存在位）
 */
typedef struct {
    uint16_t base_low;      /* 处理程序地址低 16 位 */
    uint16_t selector;      /* 代码段选择子（内核用 0x08） */
    uint8_t  zero;          /* 保留，必须为 0 */
    uint8_t  flags;         /* 类型与属性（P=0x80, DPL=0x60, type=0x0E 中断门） */
    uint16_t base_high;     /* 处理程序地址高 16 位 */
} __attribute__((packed)) idt_entry_t;

/**
 * idtr_t - IDTR 寄存器加载结构
 * 用于 lidt 指令。limit 是 IDT 总字节数减 1，base 是 IDT 的线性地址。
 */
typedef struct {
    uint16_t limit;         /* IDT 大小 - 1 */
    uint32_t base;          /* IDT 基址 */
} __attribute__((packed)) idtr_t;

/**
 * idt_init - 初始化 IDT
 *
 * 设置：
 *   1. 异常 0-19：使用异常处理程序（DPL=0，仅内核触发）
 *   2. IRQ 32-47：使用 IRQ 处理程序（DPL=0）
 *   3. 系统调用 0x80：使用 syscall 处理程序（DPL=3，用户态可触发）
 *   4. 其他（20-31, 48-255）：指向默认 stub，打印未知中断然后 EOI
 *
 * 最后执行 lidt 加载 IDTR 使设置生效。
 */
void idt_init(void);

/**
 * idt_set_gate - 设置一个 IDT 门
 * @num:     中断向量号（0~255）
 * @handler: 处理程序函数的线性地址
 * @selector: 代码段选择子
 * @flags:    类型/属性字节（bit 7=present, bit 5-6=DPL, bit 0-3=type）
 */
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags);

#endif
