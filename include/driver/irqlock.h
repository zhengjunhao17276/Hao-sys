#ifndef IRQLOCK_H
#define IRQLOCK_H

/*
 * irqlock.h - 临界区原语（配合内核态抢占）
 *
 * 单 CPU 上保护共享内核状态（VGA 光标/显存、FAT 缓冲等）：
 * 进入临界区保存 EFLAGS 后 cli（IF=0），退出时恢复原 EFLAGS。
 *
 * 为什么这样能配合抢占：
 *   - 持锁期间 IF=0 → IRQ0（PIT 抢占源）被屏蔽 → 持锁者永远不会被
 *     抢占 → 临界区原子执行，也不会出现「持锁者被抢、新任务自旋等
 *     锁」的单 CPU 死锁。
 *   - 中断 handler 本就跑在 IF=0（硬件自动 cli），不拿锁也原子。
 *   - pushfl/popfl 配对，嵌套安全：内层恢复的是内层保存的 EFLAGS
 *     （IF 仍为 0），外层才恢复原始状态。
 *
 * ⚠️ 约束：临界区内别做长操作（如阻塞式磁盘等待）——屏蔽 IRQ0 期间
 * 系统失去时钟与调度。FAT 的磁盘 PIO 是微秒级，可接受。
 */

#include <stdint.h>

/** 关中断，返回原 EFLAGS（传给 irq_unlock 恢复用） */
static inline uint32_t irq_lock(void) {
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

/** 恢复中断状态 */
static inline void irq_unlock(uint32_t flags) {
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags) : "memory");
}

#endif /* IRQLOCK_H */
