/*
 * vga.h - VGA 文本模式输出驱动接口（80×25，显存 0xB8000）
 * 支持保护区域：把已输出的历史内容设为只读，新输出在其后显示。
 */

#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stddef.h>

/* 显示尺寸常量 */
#define VGA_WIDTH  80   /* 每行 80 个字符 */
#define VGA_HEIGHT 25   /* 共 25 行 */
#define VGA_ADDR   ((uint16_t*)0xB8000)  /* VGA 文本模式显存基址 */

/* ---- 基础输出函数 ---- */
void vga_init(void);                    /* 初始化：清屏 + 启用光标 */
void vga_putchar(char c);              /* 输出单个字符（含换行/退格等转义处理） */
void vga_write(const char *str);       /* 输出字符串 */
void vga_write_color(const char *str, uint8_t color);  /* 带颜色的字符串输出 */
void vga_write_hex(uint32_t val);      /* 32 位整数按十六进制输出（如 0x1234ABCD） */
void vga_clear(void);                  /* 清屏并将光标重置到 (0,0) */
void vga_update_cursor(void);          /* 根据当前光标位置更新硬件光标位置 */

/* ---- 保护区域机制 ---- */
/* 设置保护边界：边界之前的内容只读，新输出不能覆盖 */
void vga_set_protected_position(int row, int col);
void vga_disable_protection(void);     /* 禁用保护区域，恢复正常输出 */

/* 将当前光标位置设为保护边界，常用于输出一段内容后锁定它 */
void vga_protect_before_cursor(void);

/* ---- 光标移动 ---- */
/* 在保护区域约束下上下左右移动光标，不会移入保护区域 */
void vga_move_left(void);
void vga_move_right(void);
void vga_move_up(void);
void vga_move_down(void);

/* 绝对定位光标：越界截断 + 保护区域检查 */
void vga_set_cursor(int row, int col);

/* ---- 光标显示控制 ---- */
void vga_disable_cursor(void);         /* 禁用硬件光标（消除闪烁） */
void vga_enable_cursor(void);          /* 启用硬件光标 */

/* ---- 颜色与光标状态（settings TUI 用） ---- */
void vga_set_default_color(uint8_t color);  /* 设置默认颜色（属性字节） */
uint8_t vga_get_default_color(void);        /* 读取默认颜色 */
uint32_t vga_get_cursor_pos(void);          /* 读光标位置，返回 (行<<16)|列 */

/* ---- 提示区域（用于 Shell 输入提示） ---- */
void vga_set_prompt(int row, int col, int length);       /* 设置提示区域 */
void vga_clear_prompt(void);                              /* 清除提示区域 */
void vga_protect_last_output(int length);                 /* 保护最近输出的若干字符 */

#endif
