/*
 * mouse.h - 鼠标驱动接口（PS/2 + USB 双后端，USB 断开自动回退 PS/2）
 * 坐标 0~79 × 0~24，与 VGA 文本模式匹配。
 */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>
#include <stdint.h>

/* 初始化：先 USB（支持热插拔）后 PS/2，都没有则不可用 */
void mouse_init(void);

/* 读鼠标状态到 x/y/buttons（bit0 左键、bit1 右键、bit2 中键）；
 * 关中断读共享状态，不可用时返回全 0 */
void mouse_get_packet(int *x, int *y, int *buttons);

/* 喂一个 PS/2 原始字节给协议解析器（键盘排空循环分流时调用） */
void mouse_feed_byte(uint8_t data);

/* 消费 PS/2 缓冲中的鼠标字节，保证状态最新 */
void mouse_poll(void);

/* 灵敏度 1~16 */
int mouse_get_sensitivity(void);

/* 设置灵敏度（限 1~16） */
void mouse_set_sensitivity(int sens);

/* 当前指针图案（CP437 字形码） */
uint8_t mouse_get_pointer_glyph(void);

/* 设置指针图案（CP437 字形码，如 0xDB=█） */
void mouse_set_pointer_glyph(uint8_t g);

/* 擦除指针、恢复原字符；vga 输出前调用防踩踏 */
void mouse_pointer_erase(void);

#endif
