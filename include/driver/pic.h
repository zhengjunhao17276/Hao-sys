/*
 * pic.h - 8259A 中断控制器接口（主片 0x20/0x21，从片 0xA0/0xA1）
 * 中断向量需重映射到 0x20~0x2F，避开 CPU 异常号。
 */

#ifndef PIC_H
#define PIC_H

#include <stdint.h>
#include <stdbool.h>

/* ---- 8259A 端口定义 ---- */
#define PIC1_CMD  0x20    /* 主片命令端口 */
#define PIC1_DATA 0x21    /* 主片数据端口（IMR - 中断屏蔽寄存器） */
#define PIC2_CMD  0xA0    /* 从片命令端口 */
#define PIC2_DATA 0xA1    /* 从片数据端口 */

/* ---- 函数声明 ---- */

/* 重映射中断向量基址（ICW1~ICW4），覆盖 BIOS 默认值 */
void pic_remap(uint8_t offset1, uint8_t offset2);

/* 发 EOI；中断处理返回前必须调用，IRQ≥8 需给主片补发 */
void pic_send_eoi(uint8_t irq);

/* 屏蔽/开启单个 IRQ（IMR 对应位） */
void pic_mask_irq(uint8_t irq, bool mask);

/* 查 IRQ 的 ISR 位（伪中断检测用） */
bool pic_is_in_service(uint8_t irq);

/* 初始化：重映射 0x20/0x28，只开键盘(1)、级联(2)、鼠标(12) */
void pic_init(void);

#endif
