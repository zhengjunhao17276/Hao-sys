/**
 * =========================================================================
 * mouse.h - 鼠标驱动接口（统一 PS/2 + USB）
 *
 * 提供统一的鼠标数据获取接口，底层自动检测使用 PS/2 鼠标还是 USB HID
 * 鼠标。内部实现了后端切换——如果 USB 鼠标断开连接，会自动回退到
 * PS/2 鼠标（如果存在）。
 *
 * 状态数据：
 *   - x, y: 鼠标位置（像素累计模式，范围 0-79 × 0-24，与 VGA 文本
 *     模式坐标匹配，便于在 VGA 终端显示）
 *   - buttons: 鼠标按钮状态的位掩码
 * =========================================================================
 */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * mouse_init - 初始化鼠标驱动
 *
 * 尝试顺序：
 *   1. 先探测 USB 鼠标（优先级高，因为 USB 支持热插拔）
 *   2. 如果 USB 不存在，回退到 PS/2 鼠标
 *   3. 如果两者都不存在，设置 mouse_available = false
 */
void mouse_init(void);

/**
 * mouse_get_packet - 获取当前鼠标状态
 * @x:       输出参数，鼠标 X 坐标（0~79，VGA 列范围）
 * @y:       输出参数，鼠标 Y 坐标（0~24，VGA 行范围）
 * @buttons: 输出参数，按钮状态位掩码
 *           - bit0 = 左键
 *           - bit1 = 右键
 *           - bit2 = 中键
 *
 * 内部会关中断后读取共享状态（避免中断处理程序同时修改），
 * 读取完成后恢复中断。如果鼠标不可用，返回全部 0。
 */
void mouse_get_packet(int *x, int *y, int *buttons);

/**
 * mouse_feed_byte - 向 PS/2 鼠标协议解析器喂一个字节
 * @data: 从 PS/2 数据端口（0x60）读出的原始字节
 *
 * 由键盘轮询循环在分流时调用：PS/2 状态寄存器 bit 5 (0x20)
 * 表示输出缓冲里是鼠标数据，需要交给鼠标而不是当扫描码处理。
 */
void mouse_feed_byte(uint8_t data);

/**
 * mouse_poll - 轮询式消费 PS/2 鼠标数据
 *
 * 读取前调用，确保 x/y/buttons 是最新的。
 */
void mouse_poll(void);

/**
 * mouse_get_sensitivity - 读取当前鼠标灵敏度（1~16）
 */
int mouse_get_sensitivity(void);

/**
 * mouse_set_sensitivity - 设置鼠标灵敏度（自动限制 1~16）
 * @sens: 目标灵敏度
 */
void mouse_set_sensitivity(int sens);

/**
 * mouse_get_pointer_glyph - 读取鼠标指针图案（CP437 字形码）
 */
uint8_t mouse_get_pointer_glyph(void);

/**
 * mouse_set_pointer_glyph - 设置鼠标指针图案
 * @g: CP437 字形码，如 0xDB=█、0x10=►
 */
void mouse_set_pointer_glyph(uint8_t g);

/**
 * mouse_pointer_erase - 擦除鼠标指针（恢复被覆盖的字符）
 *
 * 由 vga.c 在一切文本输出前调用，防止输出字符和指针互相踩踏。
 */
void mouse_pointer_erase(void);

#endif
