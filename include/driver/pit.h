/*
 * pit.h - 8254 PIT 定时器驱动接口（通道 0 产生 IRQ0，抢占调度时钟源）
 */

#ifndef PIT_H
#define PIT_H

#include <stdint.h>

/* 初始化通道 0：模式 3 方波，IRQ0 频率 freq_hz */
void pit_init(uint32_t freq_hz);

/* 开机以来的 tick 数 */
uint32_t pit_get_ticks(void);

/* IRQ0 处理（isr_asm.asm 调用）。⚠️ 必须先发 EOI 再调度：
 * schedule() 切走后本函数不再返回，宏尾部 EOI 不会执行，PIC 会卡死 */
void pit_tick_handler(int from_user);

#endif
