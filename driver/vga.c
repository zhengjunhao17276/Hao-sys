/**
 * =========================================================================
 * vga.c - VGA 文本模式输出驱动
 *
 * VGA 文本模式的显存位于物理地址 0xB8000，是一个 80 列 × 25 行的
 * 字符数组。每个字符占用 2 字节：
 *   低位字节 = 字符的 ASCII 码
 *   高位字节 = 属性字节（bit 0-3 = 前景色, bit 4-6 = 背景色, bit 7 = 闪烁）
 *
 * 光标位置控制：
 *   通过 VGA CRTC 寄存器（端口 0x3D4/0x3D5）的索引 0x0F（光标位置低 8 位）
 *   和 0x0E（光标位置高 8 位）控制。
 *
 * 保护区域机制：
 *   这是一个实用功能——设置一个"保护边界"后，边界之前的所有内容变为
 *   只读。滚动时只滚动保护区域以下的行，换行也不会退回到保护区域。
 *   设计用于 Shell 交互：保护历史输出不会被新输出覆盖或推走。
 * =========================================================================
 */

#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/driver/mouse.h"
#include <stddef.h>
#include <stdbool.h>

/* 光标当前位置 */
static int cursor_row = 0, cursor_col = 0;

/* 默认前景色：0x0F = 亮白色（黑底白字） */
static uint8_t default_color = 0x0F;

/* 硬件光标可见性标志（当前未使用，保留供后续光标闪烁控制） */
static bool cursor_visible __attribute__((unused)) = true;

/* ==================== 保护区域状态 ==================== */
static bool protection_enabled = false;
static int protected_row = 0;   /* 保护区域的行边界 */
static int protected_col = 0;   /* 保护区域的列边界 */

/**
 * is_position_protected - 判断指定位置是否在保护区域内
 *
 * 保护区域的定义：行 < protected_row 或 (行 == protected_row 且 列 < protected_col)
 * 保护区域内的字符不能被修改或覆盖。
 */
static bool is_position_protected(int row, int col) {
    if (!protection_enabled) return false;
    if (row < protected_row) return true;
    if (row == protected_row && col < protected_col) return true;
    return false;
}

/* ==================== 硬件光标控制 ==================== */

/**
 * vga_update_cursor - 根据当前 cursor_row/cursor_col 更新硬件光标位置
 *
 * 向 VGA CRTC 寄存器写入光标位置（80 × row + col）。
 */
void vga_update_cursor(void) {
    uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;
    outb(0x3D4, 0x0F);                         /* 选择光标位置低位寄存器 */
    outb(0x3D5, (uint8_t)(pos & 0xFF));        /* 写入低位 */
    outb(0x3D4, 0x0E);                         /* 选择光标位置高位寄存器 */
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF)); /* 写入高位 */
}

/* ==================== 初始化和清屏 ==================== */

/**
 * vga_disable_cursor - 禁用硬件光标
 *
 * VGA CRTC 寄存器 0x0A（光标起始寄存器）的 bit 5 控制光标使能：
 *   bit 5 = 1 → 禁用光标
 *   bit 5 = 0 → 启用光标
 */
void vga_disable_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);   /* bit 5 = 1 禁用光标 */
}

/**
 * vga_enable_cursor - 启用硬件光标
 *
 * 清除 bit 5 以启用光标，并用寄存器 0x0B 设置光标形状。
 */
void vga_enable_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);   /* 启用光标 */
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);   /* 光标形状：从顶到底 */
}

/**
 * vga_init - 初始化 VGA 显示子系统
 * 清屏并启用硬件光标。
 */
void vga_init(void) {
    vga_clear();
    vga_enable_cursor();
}

/**
 * vga_clear - 清空屏幕（所有字符填空格）并重置光标到 (0,0)
 */
void vga_clear(void) {
    mouse_pointer_erase();   /* 指针先擦掉，避免残留 */
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_ADDR[i] = ((uint16_t)default_color << 8) | ' ';
    cursor_row = 0;
    cursor_col = 0;
    protection_enabled = false;
    vga_update_cursor();
}

/* ==================== 屏幕滚动 ==================== */

/**
 * scroll - 当光标超出屏幕底部时向上滚动
 *
 * 分两种情况：
 *   1. 未启用保护区域：整屏上移一行，顶部行被丢弃
 *   2. 启用保护区域：只滚动保护边界以下的部分，保护区域保持不变
 */
static void scroll(void) {
    if (cursor_row < VGA_HEIGHT) return;  /* 还没超出屏幕，不需滚动 */

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
    /* 最后一行清空 */
    for (int col = 0; col < VGA_WIDTH; col++) {
        VGA_ADDR[(VGA_HEIGHT-1)*VGA_WIDTH + col] = ((uint16_t)default_color << 8) | ' ';
    }
    if (protected_row > 0) protected_row--;
    cursor_row = VGA_HEIGHT - 1;

    vga_update_cursor();
}

/* ==================== 内部核心函数 ==================== */

/**
 * putchar_core - VGA 输出核心函数（带颜色和权限检查）
 * @c:     要输出的字符
 * @color: 颜色属性
 *
 * 处理 \n（换行）、\r（回车）、\b（退格）、\t（制表符）转义序列，
 * 以及在保护区域边界处的约束行为。
 */
static void putchar_core(char c, uint8_t color) {
    /* 任何文本输出前先擦掉鼠标指针，避免指针和输出字符互相踩踏 */
    mouse_pointer_erase();

    if (c == '\n') {
        /* 换行：行号 +1，列号归 0，但不允许进入保护区域 */
        int new_row = cursor_row + 1;
        int new_col = 0;
        if (is_position_protected(new_row, new_col)) {
            return;  /* 保护区域内的换行被忽略 */
        }
        cursor_row = new_row;
        cursor_col = new_col;

    } else if (c == '\r') {
        /* 回车：列号归 0 */
        if (is_position_protected(cursor_row, 0)) {
            return;
        }
        cursor_col = 0;

    } else if (c == '\b') {
        /* 退格：向左移动一格，清除该位置的字符 */
        int target_row = cursor_row;
        int target_col = cursor_col;

        /* 计算退格的目标位置 */
        if (cursor_col > 0) {
            target_col = cursor_col - 1;
        } else if (cursor_row > 0) {
            /* 到上一行末尾——找到最后一个非空格字符的位置 */
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
            return;  /* 已经在左上角，无法退格 */
        }

        /* 检查目标位置是否受保护 */
        if (is_position_protected(target_row, target_col)) {
            return;
        }

        /* 执行退格 */
        cursor_row = target_row;
        cursor_col = target_col;
        VGA_ADDR[cursor_row * VGA_WIDTH + cursor_col] = ((uint16_t)color << 8) | ' ';
        vga_update_cursor();
        return;

    } else if (c == '\t') {
        /* 制表符：补足到下一个 4 字符边界 */
        int spaces = 4 - (cursor_col % 4);
        for (int i = 0; i < spaces; i++) {
            putchar_core(' ', color);
        }
        return;

    } else {
        /* 普通可打印字符：直接写入显存 */
        if (is_position_protected(cursor_row, cursor_col)) {
            return;  /* 保护区域内不能写入 */
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

    scroll();        /* 如果超出屏幕则滚动 */
    vga_update_cursor();
}

/* ==================== 对外接口 ==================== */

void vga_putchar(char c) {
    putchar_core(c, default_color);
}

void vga_write(const char *str) {
    while (*str) vga_putchar(*str++);
}

void vga_write_color(const char *str, uint8_t color) {
    while (*str) putchar_core(*str++, color);
}

/**
 * vga_write_hex - 将 32 位整数格式化为十六进制输出（如 0x1BADB002）
 *
 * 从最低 4 位开始处理，每次取一个 nibble（4 位）转换。
 */
void vga_write_hex(uint32_t val) {
    char hex[] = "0x00000000";
    for (int i = 9; i >= 2; i--) {
        uint8_t nibble = val & 0xF;
        hex[i] = (nibble < 10) ? '0' + nibble : 'A' + (nibble - 10);
        val >>= 4;
    }
    vga_write(hex);
}

/* ==================== 保护区域接口 ==================== */

/**
 * vga_set_default_color - 设置默认字符颜色（属性字节）
 * @color: 属性字节（低 4 位前景色，高 4 位背景色），如 0x0F=黑底白字
 *
 * 之后所有 vga_putchar/vga_write 输出都使用新颜色。
 * 供 settings TUI 的主题设置使用，实时生效。
 */
void vga_set_default_color(uint8_t color) {
    default_color = color;
}

/**
 * vga_get_default_color - 读取当前默认颜色（属性字节）
 */
uint8_t vga_get_default_color(void) {
    return default_color;
}

/**
 * vga_get_cursor_pos - 读取当前光标位置（打包）
 * @return (行 << 16) | 列
 */
uint32_t vga_get_cursor_pos(void) {
    return ((uint32_t)cursor_row << 16) | (uint32_t)cursor_col;
}

void vga_set_protected_position(int row, int col) {
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    protected_row = row;
    protected_col = col;
    protection_enabled = true;
}

void vga_disable_protection(void) {
    protection_enabled = false;
}

void vga_protect_before_cursor(void) {
    vga_set_protected_position(cursor_row, cursor_col);
}

/* ==================== 光标移动（带保护区域检查） ==================== */

void vga_move_left(void) {
    int new_row = cursor_row, new_col = cursor_col;
    if (cursor_col > 0) {
        new_col = cursor_col - 1;
    } else if (cursor_row > 0) {
        new_row = cursor_row - 1;
        new_col = VGA_WIDTH - 1;
    } else {
        return;
    }
    if (is_position_protected(new_row, new_col))
        return;
    cursor_row = new_row;
    cursor_col = new_col;
    vga_update_cursor();
}

void vga_move_right(void) {
    int new_row = cursor_row, new_col = cursor_col;
    if (cursor_col < VGA_WIDTH - 1) {
        new_col = cursor_col + 1;
    } else if (cursor_row < VGA_HEIGHT - 1) {
        new_row = cursor_row + 1;
        new_col = 0;
    } else {
        return;
    }
    if (is_position_protected(new_row, new_col))
        return;
    cursor_row = new_row;
    cursor_col = new_col;
    vga_update_cursor();
}

void vga_move_up(void) {
    if (cursor_row == 0) return;
    int new_row = cursor_row - 1;
    int new_col = cursor_col;
    if (is_position_protected(new_row, new_col))
        return;
    cursor_row = new_row;
    vga_update_cursor();
}

void vga_move_down(void) {
    if (cursor_row == VGA_HEIGHT - 1) return;
    int new_row = cursor_row + 1;
    int new_col = cursor_col;
    if (is_position_protected(new_row, new_col))
        return;
    cursor_row = new_row;
    vga_update_cursor();
}

/**
 * vga_set_cursor - 将光标绝对定位到指定位置（带边界和保护区域检查）
 * @row: 行（0 ~ VGA_HEIGHT-1）
 * @col: 列（0 ~ VGA_WIDTH-1）
 *
 * 超出边界自动截断；目标在保护区域内则忽略。
 * 供鼠标驱动使用——鼠标移动时让光标跟随，
 * 和键盘方向键（vga_move_*）控制光标的体验一致。
 */
void vga_set_cursor(int row, int col) {
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;

    if (is_position_protected(row, col))
        return;

    cursor_row = row;
    cursor_col = col;
    vga_update_cursor();
}

/* ==================== 提示区域存根（当前为空实现） ==================== */
void vga_set_prompt(int row, int col, int length) { (void)row; (void)col; (void)length; }
void vga_clear_prompt(void) { }
void vga_protect_last_output(int length) { (void)length; }
