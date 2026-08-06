/*
 * speaker.c - PC 扬声器驱动
 * PIT 通道 2 出方波（分频 = 1193180 / 频率），0x61 bit0/1 控开关；
 * 忙等待是粗略计时（1193 次循环 ≈ 1ms），蜂鸣提示音够用。
 */

#include "../include/driver/io.h"
#include "../include/driver/speaker.h"
#include <stdint.h>
#include <stdbool.h>

/* 粗略忙等：1.193MHz ≈ 1193 次/ms，蜂鸣不需要精确 */
static void pit_wait(uint32_t ms) {
    uint32_t ticks = ms * 1193;
    for (volatile uint32_t i = 0; i < ticks; i++) __asm__ volatile ("nop");
}

/* 蜂鸣：freq_hz=0 静默；PIT 分频 1193180/freq，时长 duration_ms */
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0) return;

    uint32_t divisor = 1193180 / freq_hz;
    /* ⚠️ 修复：divisor 必须 ≤ 0xFFFF（PIT 16 位计数器）——
     * freq < 18Hz 时旧实现截断发出错误频率。 */
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    /* 0x61 bit0/1 置位：通道 2 的方波送到扬声器 */
    outb(0x61, inb(0x61) | 0x03);

    /* 控制字 0xB6：通道 2、先低后高、模式 3 方波、二进制计数 */
    outb(0x43, 0xB6);
    outb(0x42, divisor & 0xFF);
    outb(0x42, (divisor >> 8) & 0xFF);

    pit_wait(duration_ms);   /* 蜂鸣持续时长 */

    /* 关扬声器：0x61 bit0/1 复位 */
    outb(0x61, inb(0x61) & ~0x03);
}

/* 强制关闭扬声器 */
void speaker_off(void) {
    outb(0x61, inb(0x61) & ~0x03);
}
