/**
 * =========================================================================
 * pic.h - 可编程中断控制器（8259A）驱动接口
 *
 * 硬件背景：
 *   x86 使用两片级联的 8259A PIC 处理 15 个硬件中断（IRQ 0~15）。
 *   主片端口 0x20/0x21，从片端口 0xA0/0xA1。从片通过 IRQ2 级联到主片。
 *
 * 中断重映射：
 *   上电后 PIC 默认将中断向量映射到 0-15，与 CPU 异常（Exception 0~15）
 *   冲突。所以必须通过 pic_remap() 将 IRQ 重映射到 0x20-0x2F（即
 *   IRQ0→int 0x20，IRQ1→int 0x21，...），与 CPU 异常错开。
 *
 * 中断屏蔽：
 *   每个 IRQ 可以单独屏蔽（mask）或开启（unmask）。初始化时只开启
 *   键盘（IRQ1）、级联（IRQ2）和鼠标（IRQ12），其余全部屏蔽，避免
 *   未处理的中断导致系统异常。
 * =========================================================================
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

/**
 * pic_remap - 重映射 PIC 的中断向量偏移
 * @offset1: 主片的起始中断向量号（通常设为 0x20）
 * @offset2: 从片的起始中断向量号（通常设为 0x28）
 *
 * 向 PIC 发送 ICW1-ICW4（初始化命令字序列），重新配置中断向量基址。
 * 这会覆盖 BIOS 在启动时设置的默认值。
 */
void pic_remap(uint8_t offset1, uint8_t offset2);

/**
 * pic_send_eoi - 发送中断结束（End Of Interrupt）信号
 * @irq: 要确认的 IRQ 号（0~15）
 *
 * 中断处理程序必须在返回前调用此函数，否则 PIC 不会再发送
 * 后续的中断。如果 IRQ≥8（从片），需要同时向主片和从片发送 EOI。
 */
void pic_send_eoi(uint8_t irq);

/**
 * pic_mask_irq - 屏蔽或开启单个 IRQ
 * @irq:  IRQ 号（0~15）
 * @mask: true=屏蔽该中断，false=开启
 *
 * 通过读写 IMR（中断屏蔽寄存器）控制。屏蔽的中断 CPU 收不到，
 * 但 PIC 内部会记录挂起状态。
 */
void pic_mask_irq(uint8_t irq, bool mask);

/**
 * pic_is_in_service - 查询 IRQ 的 In-Service 位（伪中断检测用）
 */
bool pic_is_in_service(uint8_t irq);

/**
 * pic_init - PIC 初始化快捷函数
 *
 * 调用 pic_remap(0x20, 0x28) 完成重映射后，屏蔽所有 IRQ 再选择性
 * 开启键盘（IRQ1）、级联（IRQ2）和鼠标（IRQ12）。
 */
void pic_init(void);

#endif
