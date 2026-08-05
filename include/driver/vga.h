/**
 * =========================================================================
 * vga.h - VGA 文本模式驱动接口
 *
 * 硬件背景：
 *   VGA 文本模式使用 80×25 的字符网格，每个字符在显存中占用 2 字节——
 *   低位字节是字符的 ASCII 码，高位字节是属性字节（前景色和背景色）。
 *   显存映射在物理地址 0xB8000，可以通过直接内存访问写入。
 *
 * 保护区域机制：
 *   本驱动实现了一个"保护区域"功能——可以将屏幕上方的已输出内容设为
 *   只读，新输出只能在保护区域之后的位置显示。这是为了实现简单的
 *   Shell 交互界面：保护历史输出不被回滚覆盖，新命令在底部显示。
 *
 * 光标控制：
 *   提供禁用/启用硬件光标的接口，以及上下左右移动光标的功能。
 *   硬件光标通过 VGA CRTC 寄存器（端口 0x3D4/0x3D5）控制。
 * =========================================================================
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
void vga_init(void);                    /* 初始化：清屏、禁用硬件光标 */
void vga_putchar(char c);              /* 输出单个字符（含换行/退格等转义处理） */
void vga_write(const char *str);       /* 输出字符串 */
void vga_write_color(const char *str, uint8_t color);  /* 带颜色的字符串输出 */
void vga_write_hex(uint32_t val);      /* 将 32 位整数格式化为十六进制输出（如 0x1234ABCD） */
void vga_clear(void);                  /* 清屏并将光标重置到 (0,0) */
void vga_update_cursor(void);          /* 根据当前光标位置更新硬件光标位置 */

/* ---- 保护区域机制 ---- */
/* 设置保护边界：行号小于 row 或同行列号小于 col 的区域为只读。
 * 后续输出不会修改这些位置的内容，滚动也只滚动保护区域以下的部分。 */
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

/* 将光标绝对定位到 (row, col)，带边界截断和保护区域检查（鼠标用） */
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
