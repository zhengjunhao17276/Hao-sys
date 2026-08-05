/**
 * =========================================================================
 * rtc.c - CMOS RTC 实时时钟驱动
 *
 * 通过 CMOS（端口 0x70/0x71）读取实时时钟。RTC 芯片（MC146818 兼容）
 * 由主板电池供电，即使关机也在走时，是系统唯一的"真实时间"来源。
 *
 * 访问方式：
 *   1. 向端口 0x70 写入要读取的寄存器号
 *   2. 从端口 0x71 读取该寄存器的值
 *
 * 端口 0x70 的 bit 7 是 NMI 使能位：
 *   - 0 = 允许 NMI 中断
 *   - 1 = 屏蔽 NMI
 * 读取 RTC 时应临时置位该位（屏蔽 NMI），防止读取过程中被中断打断
 * 导致读到不一致的时间（如秒 59 跳到 00 的瞬间）。
 *
 * 时间寄存器：
 *   0x00 秒     0x02 分     0x04 时     0x06 星期
 *   0x07 日     0x08 月     0x09 年
 *
 * RTC 内部时间用 BCD 编码存储（如 0x19 = 19），状态寄存器 B（0x0B）
 * 的 bit 2 表示是否使用二进制格式。HaoOS 按最常见的 BCD 格式解析。
 * =========================================================================
 */

#include "../include/driver/rtc.h"
#include "../include/driver/io.h"
#include <stdint.h>
#include <stdbool.h>

/* CMOS 端口 */
#define CMOS_ADDR 0x70   /* 地址/索引端口（写寄存器号） */
#define CMOS_DATA 0x71   /* 数据端口（读/写寄存器值） */

/* RTC 寄存器号 */
#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09

/**
 * rtc_read_reg - 读取一个 CMOS 寄存器
 * @reg: 寄存器号（0x00~0x3F）
 * 返回：寄存器值
 *
 * 读取期间屏蔽 NMI（0x70 的 bit 7 = 1），读完恢复。
 */
static uint8_t rtc_read_reg(uint8_t reg) {
    outb(CMOS_ADDR, reg | 0x80);   /* 屏蔽 NMI */
    uint8_t v = inb(CMOS_DATA);
    outb(CMOS_ADDR, 0x00);         /* 恢复 NMI */
    return v;
}

/**
 * bcd_to_bin - BCD 转二进制
 * @v: BCD 值（如 0x19）
 * 返回：二进制值（如 19）
 */
static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

uint8_t rtc_get_seconds(void) { return bcd_to_bin(rtc_read_reg(RTC_SECONDS)); }
uint8_t rtc_get_minutes(void) { return bcd_to_bin(rtc_read_reg(RTC_MINUTES)); }
uint8_t rtc_get_hours(void)   { return bcd_to_bin(rtc_read_reg(RTC_HOURS)); }
uint8_t rtc_get_day(void)     { return bcd_to_bin(rtc_read_reg(RTC_DAY)); }
uint8_t rtc_get_month(void)   { return bcd_to_bin(rtc_read_reg(RTC_MONTH)); }
uint8_t rtc_get_year(void)    { return bcd_to_bin(rtc_read_reg(RTC_YEAR)); }

/**
 * rtc_get_time_packed - 一次性读取时间（打包）
 * @return (时 << 16) | (分 << 8) | 秒
 */
uint32_t rtc_get_time_packed(void) {
    return ((uint32_t)rtc_get_hours() << 16)
         | ((uint32_t)rtc_get_minutes() << 8)
         | (uint32_t)rtc_get_seconds();
}

/**
 * rtc_get_date_packed - 一次性读取日期（打包）
 * @return ((年 - 2000) << 16) | (月 << 8) | 日
 */
uint32_t rtc_get_date_packed(void) {
    return ((uint32_t)(rtc_get_year() - 2000) << 16)
         | ((uint32_t)rtc_get_month() << 8)
         | (uint32_t)rtc_get_day();
}

/**
 * days_from_civil - 公历日期 → 自 1970-01-01 的天数（Howard Hinnant 算法）
 * 用于把 RTC 的日期时间换算成 epoch 秒，进而计算开机时长。
 */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (int)(m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/** 开机时刻的 epoch 秒（rtc_init 时记录） */
static uint32_t boot_epoch = 0;
static bool boot_epoch_set = false;

/**
 * rtc_init - 记录开机时刻（必须在开机早期调用一次）
 * 之后 rtc_get_uptime() 返回自此刻起的秒数。
 */
void rtc_init(void) {
    boot_epoch = rtc_get_epoch();
    boot_epoch_set = true;
}

/**
 * rtc_get_epoch - 读取当前 RTC 时间并换算成 epoch 秒
 * @return 自 1970-01-01 00:00:00 UTC 以来的秒数
 */
uint32_t rtc_get_epoch(void) {
    /* rtc_get_year() 返回两位数年份（如 26），需补全世纪 */
    uint32_t d = days_from_civil(2000 + rtc_get_year(), rtc_get_month(), rtc_get_day());
    return d * 86400u
         + (uint32_t)rtc_get_hours() * 3600u
         + (uint32_t)rtc_get_minutes() * 60u
         + (uint32_t)rtc_get_seconds();
}

/**
 * rtc_get_uptime - 自开机以来的秒数
 * 第一次调用时记录开机时刻，之后每次调用取差值。
 * 注意：无 PIT/无中断，RTC 是唯一时间源，秒级精度。
 */
uint32_t rtc_get_uptime(void) {
    if (!boot_epoch_set) {
        boot_epoch = rtc_get_epoch();
        boot_epoch_set = true;
    }
    return rtc_get_epoch() - boot_epoch;
}
