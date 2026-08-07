/*
 * keyboard.h - 键盘驱动接口（PS/2 中断 + USB 轮询，底层自动选择）
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>

/* 初始化：清 PS/2 缓冲、探测 USB 键盘 */
void keyboard_init(void);

/* 非阻塞：是否有按键待读取 */
bool keyboard_have_key(void);

/* 非阻塞取键：有键立即返回键值（含方向键特殊码 0x01-0x04），
 * 无键返回 -1。⚠️ 键盘 IRQ 被屏蔽（纯轮询模式），ps2_buffer 只有
 * keyboard_get_char() 的轮询循环才填充——本函数直接查 0x64/0x60
 * 端口，按 AUX 分流（鼠标字节喂鼠标解析器）。TUI 事件循环用。 */
char keyboard_get_char_nb(void);

/* 阻塞取一个按键字符：PS/2 缓冲 → USB 轮询 → 都没有则 hlt 等中断唤醒 */
char keyboard_get_char(void);

/* USB 键盘初始化（keyboard_init 内部调用） */
void usb_keyboard_init(void);

#endif
