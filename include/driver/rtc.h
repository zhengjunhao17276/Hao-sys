/*
 * rtc.h - CMOS RTC 实时时钟驱动接口（电池供电，关机也走时）
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
uint8_t rtc_get_year(void);      /* 年（两位数，如 26） */

/* 打包时间：(时<<16)|(分<<8)|秒 */
uint32_t rtc_get_time_packed(void);

/* 打包日期：(年-2000)<<16 | 月<<8 | 日 */
uint32_t rtc_get_date_packed(void);

/* 记录开机时刻（开机早期调一次） */
void rtc_init(void);

/* 当前时间 → epoch 秒 */
uint32_t rtc_get_epoch(void);

/* 自开机秒数（首次调用时记录开机时刻） */
uint32_t rtc_get_uptime(void);

#endif
