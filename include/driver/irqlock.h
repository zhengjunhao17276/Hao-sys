#ifndef IRQLOCK_H
#define IRQLOCK_H

/*
 * ============================================================
 * irqlock.h - 临界区保护原语（内核态抢占的基础设施）
 *
 * 单 CPU 上保护共享内核状态（VGA 光标/显存、FAT 缓冲等）的方式：
 *   进入临界区：保存 EFLAGS → cli（IF=0）
 *   退出临界区：恢复原 EFLAGS（popfl）
 *
 * 为什么这样能配合内核态抢占：
 *   - 持锁期间 IF=0 → IRQ0（PIT 抢占源）被屏蔽 → 持锁者永远不会
 *     被抢占 → 临界区原子执行，且不会出现「持锁者被抢占、新任务
 *     自旋等锁」的单 CPU 死锁。
 *   - 中断 handler 天然运行在 IF=0（硬件自动 cli），不拿锁也原子。
 *   - 嵌套安全：pushfl/popfl 配对，内层恢复的是内层保存的 EFLAGS
 *     （IF 仍为 0），外层恢复原始状态。
 *
 * ⚠️ 约束：临界区内禁止执行长操作（如阻塞式磁盘等待）——否则
 * IRQ0 被屏蔽期间系统失去时钟与调度响应。FAT 的磁盘 PIO 很快
 * （微秒级），可接受。
 * ============================================================
 */

#include <stdint.h>

/** 进入临界区：保存 EFLAGS 并关中断，返回需传给 irq_unlock 的旧值 */
static inline uint32_t irq_lock(void) {
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

/** 退出临界区：恢复中断状态（配合 irq_lock 的返回值） */
static inline void irq_unlock(uint32_t flags) {
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags) : "memory");
}

#endif /* IRQLOCK_H */
