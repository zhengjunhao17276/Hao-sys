/**
 * =========================================================================
 * speaker.c - PC 扬声器驱动
 *
 * PC 扬声器是一个古老的硬件接口——一个简单的方波发生器，通过 PIT
 * 通道 2 控制频率，通过端口 0x61 的 bit 0 和 bit 1 控制开关。
 *
 * 实现思路：
 *   1. 计算 PIT 分频系数 = 1193180 / 目标频率（Hz）
 *   2. 设置 PIT 控制字 0xB6（通道 2，模式 3，二进制计数）
 *   3. 向 PIT 数据端口 0x42 写入分频系数（低位→高位）
 *   4. 将端口 0x61 的 bit 0 和 bit 1 置位，使能扬声器
 *   5. 忙等待 duration_ms 毫秒
 *   6. 复位端口 0x61 的 bit 0 和 bit 1，关闭扬声器
 *
 * 注意：忙等待使用 1.193MHz 的粗略计时（1193 次循环 ≈ 1ms），
 * 不是精确的计时器——但对于蜂鸣提示音已经足够。
 * =========================================================================
 */

#include "../include/driver/io.h"
#include "../include/driver/speaker.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * pit_wait - 通过 PIT 时钟粗略忙等待
 * @ms: 等待毫秒数
 *
 * 使用 PIT 的输入频率 1.1931816MHz ≈ 1193 次/毫秒，
 * 执行 NOP 循环近似延时。不是精确延时，但蜂鸣音不需要毫秒级精度。
 */
static void pit_wait(uint32_t ms) {
    /* 约 1.193MHz / 1000 ≈ 1193 个时钟周期 ≈ 1ms */
    uint32_t ticks = ms * 1193;
    for (volatile uint32_t i = 0; i < ticks; i++) __asm__ volatile ("nop");
}

/**
 * speaker_beep - 让 PC 扬声器发出指定频率和持续时间的蜂鸣音
 * @freq_hz:     频率（Hz），例如 800 = 800Hz
 * @duration_ms: 持续时间（毫秒），例如 100 = 100ms
 *
 * 如果 freq_hz 为 0，函数直接返回（不做任何操作）。
 * 注意：频率越高，声音越尖锐。人耳听觉范围约 20Hz ~ 20kHz。
 */
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0) return;

    /* PIT 输入时钟 1.1931816 MHz，除以目标频率得到分频数 */
    uint32_t divisor = 1193180 / freq_hz;
    /* ⚠️ 修复：divisor 必须 ≤ 0xFFFF（PIT 16 位计数器）——
     * freq < 18Hz 时旧实现截断发出错误频率。 */
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    /* 启用扬声器：将端口 0x61 的 bit 0（定时器门控）和 bit 1（扬声器数据）
     * 置位。bit 0 允许 PIT 通道 2 输出方波，bit 1 将方波信号送到扬声器。 */
    outb(0x61, inb(0x61) | 0x03);

    /* 设置 PIT 通道 2（端口 0x42）：
     * 控制字 0xB6 = bit 7-6=11（通道 2）, bit 5-4=11（先低字节后高字节）,
     * bit 3-1=011（模式 3 - 方波发生器）, bit 0=0（二进制计数） */
    outb(0x43, 0xB6);
    outb(0x42, divisor & 0xFF);         /* 分频系数低字节 */
    outb(0x42, (divisor >> 8) & 0xFF);  /* 分频系数高字节 */

    /* 忙等待指定的毫秒数——在这段时间内扬声器持续发声 */
    pit_wait(duration_ms);

    /* 关闭扬声器：复位端口 0x61 的 bit 0 和 bit 1 */
    outb(0x61, inb(0x61) & ~0x03);
}

/**
 * speaker_off - 强制关闭 PC 扬声器
 */
void speaker_off(void) {
    outb(0x61, inb(0x61) & ~0x03);
}
