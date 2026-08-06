/*
 * pic.c - 8259A 中断控制器驱动
 * 初始化：ICW1~ICW4（重映射向量、级联、8086 模式）；
 * 中断处理完必须发 EOI（0x20），从片中断还要给主片补发。
 */

#include "../include/driver/io.h"
#include "../include/driver/pic.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>

/* ICW1 初始化命令：0x11 表示边沿触发、级联模式、需要 ICW4 */
#define PIC1_ICW1 0x11
#define PIC2_ICW1 0x11

/* 重映射中断向量（避开 CPU 异常号）；全程屏蔽中断防配置中途被打断 */
void pic_remap(uint8_t offset1, uint8_t offset2) {
    /* 保存当前的 IMR 值，初始化完成后恢复 */
    uint8_t mask1 = 0xFF, mask2 = 0xFF;
    mask1 = inb(PIC1_DATA);
    mask2 = inb(PIC2_DATA);

    /* ICW1：初始化开始 */
    outb(PIC1_CMD, 0x11);   /* 主片 */
    outb(PIC2_CMD, 0x11);   /* 从片 */
    /* ICW2：向量基址 */
    outb(PIC1_DATA, offset1);  /* 主片 IRQ0~7 → offset1+ */
    outb(PIC2_DATA, offset2);  /* 从片 IRQ8~15 → offset2+ */
    /* ICW3：级联 */
    outb(PIC1_DATA, 0x04);  /* 主片：从片走 IRQ2 */
    outb(PIC2_DATA, 0x02);  /* 从片：级联 ID=2 */
    /* ICW4：8086 模式 */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

/* 发 EOI（0x20）；IRQ≥8 时从片中断还要给主片补发（走 IRQ2 级联） */
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);  /* 从片 */
    outb(PIC1_CMD, 0x20);                /* 主片 */
}

/* 改 IMR 对应位：置位=屏蔽，清零=开启 */
void pic_mask_irq(uint8_t irq, bool mask) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq % 8);
    uint8_t val = inb(port);
    if (mask) {
        val |= (1 << bit);
    } else {
        val &= ~(1 << bit);
    }
    outb(port, val);
}

/* 查 ISR 位（OCW3 写 0x0B 读回）。伪中断 IRQ7/15 时 ISR 未置位，不能发 EOI */
bool pic_is_in_service(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_CMD : PIC2_CMD;
    outb(port, 0x0B);              /* OCW3：读 ISR */
    uint8_t isr = inb(port);
    return (isr >> (irq % 8)) & 1;
}

/* 重映射到 0x20/0x28（避开 CPU 异常号），只开键盘(1)、级联(2)、鼠标(12) */
void pic_init(void) {
    pic_remap(0x20, 0x28);

    /* 屏蔽所有中断，再选择性开启需要的 */
    for (int i = 0; i < 16; i++) pic_mask_irq(i, true);

    pic_mask_irq(1, false);   /* IRQ1 = PS/2 键盘 */
    pic_mask_irq(2, false);   /* IRQ2 = PIC 级联（从片必须通过此线） */
    pic_mask_irq(12, false);  /* IRQ12 = PS/2 鼠标 */
}
