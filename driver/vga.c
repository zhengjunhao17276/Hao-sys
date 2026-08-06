/*
 * vga.c - VGA 文本模式输出驱动
 * 显存 0xB8000（80×25）；保护区域用于把 Shell 历史输出设为只读。
 */

#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/driver/mouse.h"
#include "../include/driver/irqlock.h"
#include <stddef.h>
#include <stdbool.h>

/* 光标当前位置 */
static int cursor_row = 0, cursor_col = 0;

/* 默认前景色：0x0F = 亮白色（黑底白字） */
static uint8_t default_color = 0x0F;

/* 硬件光标可见性标志（当前未使用，保留供后续光标闪烁控制） */
static bool cursor_visible __attribute__((unused)) = true;

static bool protection_enabled = false;
static int protected_row = 0;   /* 保护区域的行边界 */
static int protected_col = 0;   /* 保护区域的列边界 */

/* 保护区域内不可写：行 < protected_row，或同行且列 < protected_col */
static bool is_position_protected(int row, int col) {
    if (!protection_enabled) return false;
    if (row < protected_row) return true;
    if (row == protected_row && col < protected_col) return true;
    return false;
}


/* 按当前光标位置更新硬件光标（CRTC 寄存器，80 × row + col） */
void vga_update_cursor(void) {
    uint32_t fl = irq_lock();
    uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;
    outb(0x3D4, 0x0F);                         /* 光标低 8 位 */
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);                         /* 光标高 8 位 */
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    irq_unlock(fl);
}


/* CRTC 0x0A 的 bit5=1 禁用硬件光标 */
void vga_disable_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

/* 清 bit5 启用光标，0x0B 设置光标形状 */
void vga_enable_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);   /* 形状：从顶到底 */
}

/* 初始化：清屏 + 启用光标 */
void vga_init(void) {
    vga_clear();
    vga_enable_cursor();
}

/* 清屏并复位光标到 (0,0) */
void vga_clear(void) {
    uint32_t fl = irq_lock();
    mouse_pointer_erase();   /* 指针先擦掉，避免残留 */
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_ADDR[i] = ((uint16_t)default_color << 8) | ' ';
    cursor_row = 0;
    cursor_col = 0;
    protection_enabled = false;
    vga_update_cursor();
    irq_unlock(fl);
}


static void scroll(void) {
    if (cursor_row < VGA_HEIGHT) return;

    /* 整屏向上滚动一行（从第 1 行开始覆盖到第 0 行）。
     * ⚠️ 历史修复：保护区域启用时曾尝试"只滚动边界以下"，但 start_row
     * 取 protected_row 会把提示符行（含刚输入的命令）一起滚掉，导致
     * 多行输出时命令和输出丢失（如 ls 输出 6 项只留 2 项）。
     * 统一语义：任何情况下整屏滚动，保护边界随之上移一行——
     * 历史随滚动走（标准终端行为），shell 打印新提示符后重新设置边界。 */
    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            VGA_ADDR[(row-1)*VGA_WIDTH + col] = VGA_ADDR[row*VGA_WIDTH + col];
        }
    }
    for (int col = 0; col < VGA_WIDTH; col++) {
        VGA_ADDR[(VGA_HEIGHT-1)*VGA_WIDTH + col] = ((uint16_t)default_color << 8) | ' ';
    }
    if (protected_row > 0) protected_row--;
    cursor_row = VGA_HEIGHT - 1;

    vga_update_cursor();
}


/* 输出核心：处理 \n \r \b \t 转义和保护区域约束 */
static void putchar_core(char c, uint8_t color) {
    /* 任何文本输出前先擦掉鼠标指针，避免指针和输出字符互相踩踏 */
    mouse_pointer_erase();

    if (c == '\n') {
        /* 换行，但不得进入保护区域 */
        int new_row = cursor_row + 1;
        int new_col = 0;
        if (is_position_protected(new_row, new_col)) {
            return;
        }
        cursor_row = new_row;
        cursor_col = new_col;

    } else if (c == '\r') {
        if (is_position_protected(cursor_row, 0)) {
            return;
        }
        cursor_col = 0;

    } else if (c == '\b') {
        int target_row = cursor_row;
        int target_col = cursor_col;

        if (cursor_col > 0) {
            target_col = cursor_col - 1;
        } else if (cursor_row > 0) {
            /* 上一行末尾：找最后一个非空格字符 */
            target_row = cursor_row - 1;
            int last_non_space = -1;
            for (int i = VGA_WIDTH - 1; i >= 0; i--) {
                if ((VGA_ADDR[target_row * VGA_WIDTH + i] & 0xFF) != ' ') {
                    last_non_space = i;
                    break;
                }
            }
            target_col = (last_non_space != -1) ? last_non_space : 0;
        } else {
            return;  /* 已在左上角，无路可退 */
        }

        if (is_position_protected(target_row, target_col)) {
            return;
        }

        cursor_row = target_row;
        cursor_col = target_col;
        VGA_ADDR[cursor_row * VGA_WIDTH + cursor_col] = ((uint16_t)color << 8) | ' ';
        vga_update_cursor();
        return;

    } else if (c == '\t') {
        /* Tab：补到下一个 4 字符边界 */
        int spaces = 4 - (cursor_col % 4);
        for (int i = 0; i < spaces; i++) {
            putchar_core(' ', color);
        }
        return;

    } else {
        if (is_position_protected(cursor_row, cursor_col)) {
            return;
        }
        VGA_ADDR[cursor_row * VGA_WIDTH + cursor_col] = ((uint16_t)color << 8) | (uint8_t)c;
        cursor_col++;
        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
            /* ⚠️ 关键修复：底行（24）右角写完 80 个字符后换行会到 row 25，
             * 触发 scroll() 把整屏滚上去——TUI 画底边框时每次绘制都滚一次，
             * 旧内容被顶行、和新内容叠成乱屏。
             * 修复：光标钳制在右下角，不越界、不滚动。 */
            if (cursor_row >= VGA_HEIGHT) {
                cursor_row = VGA_HEIGHT - 1;
                cursor_col = VGA_WIDTH - 1;
            }
        }
    }

    scroll();
    vga_update_cursor();
}


void vga_putchar(char c) {
    uint32_t fl = irq_lock();
    putchar_core(c, default_color);
    irq_unlock(fl);
}

void vga_write(const char *str) {
    /* 整串一把锁，直接走 putchar_core 避免逐字符加锁 */
    uint32_t fl = irq_lock();
    while (*str) putchar_core(*str++, default_color);
    irq_unlock(fl);
}

void vga_write_color(const char *str, uint8_t color) {
    uint32_t fl = irq_lock();
    while (*str) putchar_core(*str++, color);
    irq_unlock(fl);
}

/* 32 位整数按十六进制输出（0x00000000 格式） */
void vga_write_hex(uint32_t val) {
    char hex[] = "0x00000000";
    for (int i = 9; i >= 2; i--) {
        uint8_t nibble = val & 0xF;
        hex[i] = (nibble < 10) ? '0' + nibble : 'A' + (nibble - 10);
        val >>= 4;
    }
    vga_write(hex);
}


/* 设置默认颜色（低 4 位前景、高 4 位背景，如 0x0F = 黑底白字） */
void vga_set_default_color(uint8_t color) {
    default_color = color;
}

uint8_t vga_get_default_color(void) {
    return default_color;
}

/* 打包返回光标位置：(行 << 16) | 列 */
uint32_t vga_get_cursor_pos(void) {
    return ((uint32_t)cursor_row << 16) | (uint32_t)cursor_col;
}

void vga_set_protected_position(int row, int col) {
    uint32_t fl = irq_lock();
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    protected_row = row;
    protected_col = col;
    protection_enabled = true;
    irq_unlock(fl);
}

void vga_disable_protection(void) {
    uint32_t fl = irq_lock();
    protection_enabled = false;
    irq_unlock(fl);
}

void vga_protect_before_cursor(void) {
    uint32_t fl = irq_lock();
    vga_set_protected_position(cursor_row, cursor_col);
    irq_unlock(fl);
}


void vga_move_left(void) {
    uint32_t fl = irq_lock();
    int new_row = cursor_row, new_col = cursor_col;
    if (cursor_col > 0) {
        new_col = cursor_col - 1;
    } else if (cursor_row > 0) {
        new_row = cursor_row - 1;
        new_col = VGA_WIDTH - 1;
    } else {
        return;
    }
    if (is_position_protected(new_row, new_col)) { irq_unlock(fl); return; }
    cursor_row = new_row;
    cursor_col = new_col;
    vga_update_cursor();
    irq_unlock(fl);
}

void vga_move_right(void) {
    uint32_t fl = irq_lock();
    int new_row = cursor_row, new_col = cursor_col;
    if (cursor_col < VGA_WIDTH - 1) {
        new_col = cursor_col + 1;
    } else if (cursor_row < VGA_HEIGHT - 1) {
        new_row = cursor_row + 1;
        new_col = 0;
    } else {
        return;
    }
    if (is_position_protected(new_row, new_col)) { irq_unlock(fl); return; }
    cursor_row = new_row;
    cursor_col = new_col;
    vga_update_cursor();
    irq_unlock(fl);
}

void vga_move_up(void) {
    uint32_t fl = irq_lock();
    if (cursor_row == 0) { irq_unlock(fl); return; }
    int new_row = cursor_row - 1;
    int new_col = cursor_col;
    if (is_position_protected(new_row, new_col)) { irq_unlock(fl); return; }
    cursor_row = new_row;
    vga_update_cursor();
    irq_unlock(fl);
}

void vga_move_down(void) {
    uint32_t fl = irq_lock();
    if (cursor_row == VGA_HEIGHT - 1) { irq_unlock(fl); return; }
    int new_row = cursor_row + 1;
    int new_col = cursor_col;
    if (is_position_protected(new_row, new_col)) { irq_unlock(fl); return; }
    cursor_row = new_row;
    vga_update_cursor();
    irq_unlock(fl);
}

/* 绝对定位光标：越界截断，保护区域内忽略 */
void vga_set_cursor(int row, int col) {
    uint32_t fl = irq_lock();
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;

    if (is_position_protected(row, col)) { irq_unlock(fl); return; }

    cursor_row = row;
    cursor_col = col;
    vga_update_cursor();
    irq_unlock(fl);
}

/* 提示区域存根（当前为空实现） */
void vga_set_prompt(int row, int col, int length) { (void)row; (void)col; (void)length; }
void vga_clear_prompt(void) { }
void vga_protect_last_output(int length) { (void)length; }
