/**
 * =========================================================================
 * pit.h - 8254 可编程间隔定时器（PIT）驱动
 *
 * PIT 通道 0 产生周期性 IRQ0 中断，作为抢占式调度的时钟源。
 * =========================================================================
 */

#ifndef PIT_H
#define PIT_H

#include <stdint.h>

/**
 * pit_init - 初始化 PIT 通道 0
 * @freq_hz: 中断频率（Hz），范围约 19~1193182
 *
 * 配置为模式 3（方波），计数初值 = 1193182 / freq。
 * 每次计数归零触发一次 IRQ0。
 */
void pit_init(uint32_t freq_hz);

/**
 * pit_get_ticks - 读取自开机以来的 tick 数
 */
uint32_t pit_get_ticks(void);

/**
 * pit_tick_handler - IRQ0 中断处理函数（由 isr_asm.asm 调用）
 * @from_user: 1 = 被中断的上下文是用户态（可抢占），0 = 内核态
 *
 * ⚠️ 必须先发 EOI 再调度：schedule() 切换任务后本函数不会返回，
 * 宏尾部的 EOI 代码不会执行——PIC 收不到 EOI 会屏蔽后续全部中断。
 */
void pit_tick_handler(int from_user);

#endif
