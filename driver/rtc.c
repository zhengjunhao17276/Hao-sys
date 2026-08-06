/*
 * rtc.c - CMOS RTC 实时时钟驱动（端口 0x70/0x71）
 * 时间以 BCD 存储；读寄存器前要屏蔽 NMI（0x70 bit7），防读到撕裂值。
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

/* 读 CMOS 寄存器：期间屏蔽 NMI，读完恢复 */
static uint8_t rtc_read_reg(uint8_t reg) {
    /* ⚠️ 修复：先等待 UIP（寄存器 A bit7）清除。
     * RTC 每秒有一个约 2ms 的更新窗口，期间所有时间寄存器
     * 都可能处于撕裂状态（年份读出 0x74 而非 0x26 这类怪值）。
     * 真机上不等待会读到中间态数据；QEMU 下偶尔也会复现。 */
    for (int i = 0; i < 10000; i++) {
        outb(CMOS_ADDR, 0x0A | 0x80);          /* 读寄存器 A，屏蔽 NMI */
        if (!(inb(CMOS_DATA) & 0x80)) break;    /* UIP=0：可以安全读取 */
    }
    outb(CMOS_ADDR, reg | 0x80);   /* 屏蔽 NMI */
    uint8_t v = inb(CMOS_DATA);
    outb(CMOS_ADDR, 0x00);         /* 恢复 NMI */
    return v;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

uint8_t rtc_get_seconds(void) { return bcd_to_bin(rtc_read_reg(RTC_SECONDS)); }
uint8_t rtc_get_minutes(void) { return bcd_to_bin(rtc_read_reg(RTC_MINUTES)); }
uint8_t rtc_get_hours(void)   { return bcd_to_bin(rtc_read_reg(RTC_HOURS)); }
uint8_t rtc_get_day(void)     { return bcd_to_bin(rtc_read_reg(RTC_DAY)); }
uint8_t rtc_get_month(void)   { return bcd_to_bin(rtc_read_reg(RTC_MONTH)); }
uint8_t rtc_get_year(void)    { return bcd_to_bin(rtc_read_reg(RTC_YEAR)); }

/* 打包时间：(时<<16)|(分<<8)|秒 */
uint32_t rtc_get_time_packed(void) {
    return ((uint32_t)rtc_get_hours() << 16)
         | ((uint32_t)rtc_get_minutes() << 8)
         | (uint32_t)rtc_get_seconds();
}

/* 打包日期：(年-2000)<<16 | 月<<8 | 日 */
uint32_t rtc_get_date_packed(void) {
    /* ⚠️ 修复：rtc_get_year() 返回两位数年份（如 26，即“年-2000”）。
     * 旧代码写 (year - 2000) 会得到 26-2000 = -1974 → uint32 下溢，
     * 高 16 位变成 0xF84A，shell 取 &0xFF 得 0x4A=74 → 显示 2074。
     * 正确做法：两位数年份本身就是“年-2000”，直接放高 16 位。 */
    return ((uint32_t)rtc_get_year() << 16)
         | ((uint32_t)rtc_get_month() << 8)
         | (uint32_t)rtc_get_day();
}

/* 公历 → 1970-01-01 起的天数（Howard Hinnant 算法），换算 epoch 用 */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (int)(m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* 开机时刻的 epoch 秒（rtc_init 时记录） */
static uint32_t boot_epoch = 0;
static bool boot_epoch_set = false;

/* 记录开机时刻（开机早期调一次），供 rtc_get_uptime() 使用 */
void rtc_init(void) {
    boot_epoch = rtc_get_epoch();
    boot_epoch_set = true;
}

/* 当前 RTC 时间 → epoch 秒 */
uint32_t rtc_get_epoch(void) {
    /* rtc_get_year() 返回两位数年份（如 26），需补全世纪 */
    uint32_t d = days_from_civil(2000 + rtc_get_year(), rtc_get_month(), rtc_get_day());
    return d * 86400u
         + (uint32_t)rtc_get_hours() * 3600u
         + (uint32_t)rtc_get_minutes() * 60u
         + (uint32_t)rtc_get_seconds();
}

/* 自开机秒数（首次调用时记录开机时刻）；RTC 是唯一时间源，秒级精度 */
uint32_t rtc_get_uptime(void) {
    if (!boot_epoch_set) {
        boot_epoch = rtc_get_epoch();
        boot_epoch_set = true;
    }
    return rtc_get_epoch() - boot_epoch;
}
