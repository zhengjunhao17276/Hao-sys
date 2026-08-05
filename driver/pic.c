/**
 * =========================================================================
 * pic.c - 8259A 可编程中断控制器驱动
 *
 * 编程流程：
 *   初始化 PIC 需要发送四个初始化命令字（ICW1~ICW4）：
 *   - ICW1: 初始化开始信号（0x11 表示需要 ICW4）
 *   - ICW2: 设置中断向量基址（主片 0x20，从片 0x28）
 *   - ICW3: 级联配置（主片说明从片接在哪条 IRQ 线，从片说明自己 ID）
 *   - ICW4: 模式设置（0x01 = 8086/88 模式，非 MCS-86 模式）
 *
 * 中断屏蔽：
 *   IMR（中断屏蔽寄存器，端口 PIC1_DATA/PIC2_DATA）的每个 bit 控制
 *   对应的 IRQ——bit=1 表示屏蔽该中断。
 *
 * End of Interrupt (EOI)：
 *   中断处理程序完成后，必须通过 PIC 命令端口发送 EOI（0x20），
 *   通知 PIC 可以继续处理下一级中断。从片的中断还需要额外发送 EOI
 *   给主片（因为从片通过 IRQ2 级联到主片）。
 * =========================================================================
 */

#include "../include/driver/io.h"
#include "../include/driver/pic.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>

/* ICW1 初始化命令：0x11 表示边沿触发、级联模式、需要 ICW4 */
#define PIC1_ICW1 0x11
#define PIC2_ICW1 0x11

/**
 * pic_remap - 重映射 PIC 的中断向量偏移
 * @offset1: 主片 IRQ 0~7 映射到的中断向量号（如 0x20→IRQ0=INT 0x20）
 * @offset2: 从片 IRQ 8~15 映射到的中断向量号（如 0x28→IRQ8=INT 0x28）
 *
 * 重映射过程会暂时屏蔽所有中断（保存原 IMR 值，完成后恢复），
 * 防止在配置过程中意外触发中断。
 */
void pic_remap(uint8_t offset1, uint8_t offset2) {
    /* 保存当前的 IMR 值，初始化完成后恢复 */
    uint8_t mask1 = 0xFF, mask2 = 0xFF;
    mask1 = inb(PIC1_DATA);
    mask2 = inb(PIC2_DATA);

    /* ICW1：向命令端口发送初始化信号 0x11 */
    outb(PIC1_CMD, 0x11);   /* 主片初始化，边沿触发，级联，需要 ICW4 */
    outb(PIC2_CMD, 0x11);   /* 从片同上 */
    /* ICW2：设置中断向量基址 */
    outb(PIC1_DATA, offset1);  /* 主片 IRQ0~7 映射到 offset1~(offset1+7) */
    outb(PIC2_DATA, offset2);  /* 从片 IRQ8~15 映射到 offset2~(offset2+7) */
    /* ICW3：级联关系 */
    outb(PIC1_DATA, 0x04);  /* 主片：bit2=1 表示从片通过 IRQ2 级联 */
    outb(PIC2_DATA, 0x02);  /* 从片：级联 ID = 2（标识自己是主片的 IRQ2 下游） */
    /* ICW4：模式设置 */
    outb(PIC1_DATA, 0x01);  /* 8086 模式，自动 EOI 关闭 */
    outb(PIC2_DATA, 0x01);

    /* 恢复之前保存的 IMR 值 */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

/**
 * pic_send_eoi - 发送中断结束（EOI）信号
 * @irq: 要确认的 IRQ 号
 *
 * 向 PIC 命令端口写入 0x20 表示 EOI。如果 IRQ≥8，还需要额外向
 * 主片发送 EOI（因为从片的中断经过主片的 IRQ2 传递给 CPU）。
 */
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);  /* 从片中 EOI */
    outb(PIC1_CMD, 0x20);                /* 主片 EOI */
}

/**
 * pic_mask_irq - 屏蔽或开启单个 IRQ
 * @irq:  IRQ 号（0~15）
 * @mask: true=屏蔽，false=开启
 *
 * 通过读写 IMR（中断屏蔽寄存器）实现。IMR 在数据端口（PIC1_DATA
 * 或 PIC2_DATA）中，每个 bit 对应一个 IRQ。
 */
void pic_mask_irq(uint8_t irq, bool mask) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq % 8);
    uint8_t val = inb(port);
    if (mask) {
        val |= (1 << bit);     /* 置位 bit = 屏蔽中断 */
    } else {
        val &= ~(1 << bit);    /* 清零 bit = 开启中断 */
    }
    outb(port, val);
}

/**
 * pic_init - PIC 初始化快捷函数
 *
 * 将 PIC 重映射到 0x20/0x28（与 CPU 异常号 0-19 错开），
 * 然后默认屏蔽所有 IRQ，仅开启键盘（IRQ1）、级联（IRQ2）
 * 和鼠标（IRQ12）三个必需的中断源。
 */
void pic_init(void) {
    pic_remap(0x20, 0x28);

    /* 屏蔽所有中断，再选择性开启需要的 */
    for (int i = 0; i < 16; i++) pic_mask_irq(i, true);

    pic_mask_irq(1, false);   /* IRQ1 = PS/2 键盘 */
    pic_mask_irq(2, false);   /* IRQ2 = PIC 级联（从片必须通过此线） */
    pic_mask_irq(12, false);  /* IRQ12 = PS/2 鼠标 */
}
