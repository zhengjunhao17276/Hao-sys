/**
 * =========================================================================
 * pit.c - 8254 PIT 定时器驱动 + 抢占式调度时钟
 *
 * 抢占式调度策略（简化但安全）：
 *   - 只在用户态抢占：IRQ0 触发时若被中断的是用户态代码（ring 3），
 *     累计时间片，用尽则调用 schedule() 切换任务。
 *   - 内核态（ring 0）触发的 IRQ0 只累计 tick 计数，不调度——
 *     避免在内核代码中途（VGA/FAT/键盘缓冲等共享状态）被抢占导致重入。
 *     内核关键路径本就关中断（IF=0），此规则天然成立。
 * =========================================================================
 */

#include "../include/driver/pit.h"
#include "../include/driver/pic.h"
#include "../include/driver/io.h"
#include "../include/proc/task.h"

/* 时间片：用户任务最多连续运行 TIME_SLICE_TICKS 个 tick（100Hz 下 30ms） */
#define TIME_SLICE_TICKS 3

/* 开机以来的 tick 计数 */
static volatile uint32_t pit_ticks = 0;

void pit_init(uint32_t freq_hz) {
    /* 计数初值 = 时钟频率 1193182Hz / 目标频率 */
    uint32_t divisor = 1193182 / freq_hz;
    if (divisor < 2) divisor = 2;          /* 上限 ~596KHz */
    if (divisor > 65535) divisor = 65535;  /* 下限 ~18Hz */

    /* 命令字 0x36：通道 0，先写低字节再写高字节，模式 3（方波），二进制 */
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);            /* 低字节 */
    outb(0x40, (divisor >> 8) & 0xFF);     /* 高字节 */
}

uint32_t pit_get_ticks(void) {
    return pit_ticks;
}

void pit_tick_handler(int from_user) {
    pit_ticks++;

    /* ⚠️ 先发 EOI：schedule() 切换走后本函数不会返回，
     * 宏尾部（call 之后）的 EOI 永远不会执行 → PIC 卡死。 */
    pic_send_eoi(0);

    /* 只有用户态被中断时才考虑抢占（内核态共享状态不可重入） */
    if (from_user && current_task && current_task->is_user) {
        current_task->ticks_used++;
        if (current_task->ticks_used >= TIME_SLICE_TICKS) {
            current_task->ticks_used = 0;
            schedule();   /* 时间片用尽，抢占切换 */
        }
    }
}
