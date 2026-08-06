/*
 * pit.c - 8254 PIT 定时器 + 抢占式调度时钟源
 * IRQ0 驱动调度；内核态抢占安全性由 irqlock 保证（持锁期间 IF=0）。
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
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

uint32_t pit_get_ticks(void) {
    return pit_ticks;
}

void pit_tick_handler(int from_user) {
    pit_ticks++;

    /* ⚠️ 先发 EOI：schedule() 切换走后本函数不会返回，
     * 宏尾部（call 之后）的 EOI 永远不会执行 → PIC 卡死。 */
    pic_send_eoi(0);

    /* ⚠️ 架构升级（#4 内核态抢占）：不再限制用户态。
     * 安全性由 irqlock（VGA/FAT 等共享状态持锁期间 IF=0，IRQ0 被屏蔽，
     * 持锁者不会被抢占）保证——不持锁的内核代码可被抢占，恢复靠
     * 中断帧 iret（CS 保持 0x08 回内核态），与用户态抢占同一机制。
     * from_user 参数保留（调试用），不再作为调度条件。 */
    (void)from_user;
    if (current_task && current_task->is_user) {
        current_task->ticks_used++;
        if (current_task->ticks_used >= TIME_SLICE_TICKS) {
            current_task->ticks_used = 0;
            schedule();   /* 时间片用尽，抢占切换 */
        }
    }
}
