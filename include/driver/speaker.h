/*
 * speaker.h - PC 扬声器驱动接口（PIT 通道 2 出方波，只能单音）
 */

#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

/* 蜂鸣：指定频率和时长（freq_hz=0 静默） */
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms);

/* 强制关闭扬声器 */
void speaker_off(void);

#endif
