/**
 * =========================================================================
 * rtc.h - CMOS RTC 实时时钟驱动接口
 *
 * 提供读系统时间/日期的接口。时间源是主板 CMOS 的 RTC 芯片，
 * 由电池供电持续走时。
 * =========================================================================
 */

#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stdbool.h>

/* ---- 时间读取 ---- */
uint8_t rtc_get_seconds(void);   /* 秒 0~59 */
uint8_t rtc_get_minutes(void);   /* 分 0~59 */
uint8_t rtc_get_hours(void);     /* 时 0~23 */
uint8_t rtc_get_day(void);       /* 日 1~31 */
uint8_t rtc_get_month(void);     /* 月 1~12 */
uint8_t rtc_get_year(void);      /* 年（如 2026） */

/**
 * rtc_get_time_packed - 读取时间（打包）
 * @return (时 << 16) | (分 << 8) | 秒
 */
uint32_t rtc_get_time_packed(void);

/**
 * rtc_get_date_packed - 读取日期（打包）
 * @return ((年-2000) << 16) | (月 << 8) | 日
 */
uint32_t rtc_get_date_packed(void);

/**
 * rtc_init - 记录开机时刻（开机早期调用一次）
 */
void rtc_init(void);

/**
 * rtc_get_epoch - 读取当前 RTC 时间换算成 epoch 秒（自 1970-01-01）
 */
uint32_t rtc_get_epoch(void);

/**
 * rtc_get_uptime - 自开机以来的秒数（首次调用时记录开机时刻）
 */
uint32_t rtc_get_uptime(void);

#endif
