/**
 * =========================================================================
 * keyboard.h - 键盘驱动接口（统一 PS/2 + USB）
 *
 * 这个驱动提供统一的键盘输入接口，底层自动探测 PS/2 键盘或 USB HID
 * 键盘。外部调用者不需要关心使用哪种物理接口，只需调用 keyboard_get_char()
 * 获取按键即可。
 *
 * 内部架构：
 *   - PS/2 键盘使用中断方式：IRQ1 触发时，keyboard_irq_handler() 将
 *     扫描码存入环形缓冲区，然后 keyboard_get_char() 从中读取。
 *   - USB 键盘使用轮询方式：每次调用 keyboard_get_char() 时会触发
 *     usb_keyboard_poll() 获取 HID 报告。
 *   - 两种键盘的扫描码到 ASCII 的转换共用同一映射表，包括 Shift 和
 *     Caps Lock 状态处理。
 * =========================================================================
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>

/**
 * keyboard_init - 初始化键盘驱动
 *
 * 清空 PS/2 键盘控制器的缓冲，探测 USB 键盘，输出初始化信息。
 */
void keyboard_init(void);

/**
 * keyboard_have_key - 非阻塞检查是否有按键缓冲
 * 返回 true 表示有按键等待读取
 */
bool keyboard_have_key(void);

/**
 * keyboard_get_char - 阻塞式获取一个键盘输入字符
 *
 * 依次检查 PS/2 环形缓冲区、USB 键盘报告，如果都没有则执行 hlt
 * 进入低功耗等待，直到下一个中断唤醒。返回转换后的 ASCII 字符。
 */
char keyboard_get_char(void);

/**
 * usb_keyboard_init - USB 键盘初始化（由 keyboard_init 内部调用）
 */
void usb_keyboard_init(void);

#endif
