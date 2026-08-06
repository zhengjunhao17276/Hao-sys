/*
 * keyboard.c - 统一键盘驱动（PS/2 中断 + USB 轮询）
 * 扫描码 → ASCII 转换（Make < 0x80，Break = Make + 0x80）；
 * 方向键拦截用于 VGA 光标移动，不作为字符返回。
 */

#include "../include/driver/keyboard.h"
#include "../include/driver/pic.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/driver/usb.h"
#include "../include/driver/mouse.h"
#include <stdint.h>
#include <stdbool.h>

bool shift_pressed = false;      /* Shift 键是否被按下 */
bool caps_lock = false;          /* Caps Lock 是否锁定 */

#define PS2_BUFFER_SIZE 64       /* 环形缓冲区大小 */
static volatile uint8_t ps2_buffer[PS2_BUFFER_SIZE];  /* 扫描码缓冲区 */
static volatile uint16_t ps2_head = 0;   /* 读取位置（消费者索引） */
static volatile uint16_t ps2_tail = 0;   /* 写入位置（生产者索引） */

/* 扫描码 → ASCII 映射（非 Shift，IBM PC/AT Set 1） */
static const char normal_map[128] = {
    [0x01] = 0x1B,              /* Esc */
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b',              /* Backspace */
    [0x0F] = '\t',              /* Tab */
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',              /* Enter */
    [0x1D] = 0,                 /* Left Ctrl */
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2A] = 0,                 /* Left Shift */
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c',
    [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = 0,                 /* Right Shift */
    [0x37] = '*',               /* PrtSc/SysRq */
    [0x38] = 0,                 /* Left Alt */
    [0x39] = ' ',               /* Space */
};

/* Shift 状态映射：字母大写、数字变符号 */
static const char shift_map[128] = {
    [0x01] = 0x1B,              /* Esc */
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b',              /* Backspace */
    [0x0F] = '\t',              /* Tab */
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1D] = 0,                 /* Left Ctrl */
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2A] = 0,                 /* Left Shift */
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C',
    [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x36] = 0,                 /* Right Shift */
    [0x37] = '*',
    [0x38] = 0,                 /* Left Alt */
    [0x39] = ' ',
};


/* 大小写：Shift 与 Caps Lock 是 XOR 关系，非字母不受影响 */
static char apply_caps_lock(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        bool want_upper = shift_pressed ^ caps_lock;  /* XOR */
        if (want_upper) {
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        } else {
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        }
    }
    return c;
}


/* 扫描码转 ASCII；0 = 修饰键/释放事件。副作用：更新 shift/caps 状态 */
static char ps2_scancode_to_ascii(uint8_t sc) {
    if (sc & 0x80) {
        /* bit 7 = 1 → 按键释放事件 */
        uint8_t key = sc & 0x7F;
        if (key == 0x2A || key == 0x36) {
            shift_pressed = false;  /* Left/Right Shift 释放 */
        }
        return 0;
    }

    if (sc == 0x2A || sc == 0x36) {
        shift_pressed = true;       /* Shift 按下 */
        return 0;
    }

    if (sc == 0x3A) {               /* Caps Lock 切换 */
        caps_lock = !caps_lock;
        return 0;
    }

    const char *map = shift_pressed ? shift_map : normal_map;
    char c = 0;
    if (sc < sizeof(normal_map)) {
        c = map[sc];
    }

    if (c) c = apply_caps_lock(c);
    return c;
}


/* IRQ1 中断处理：扫描码入环形缓冲后发 EOI（中断上下文，只能碰 volatile） */
void keyboard_irq_handler(void) {
    /* ⚠️ 与 ps2_mouse_irq_handler 同理：getchar 的读空循环（cli 下）
     * 可能已把 FIFO 清空，此处 inb(0x60) 会读到空 FIFO 垃圾。
     * 先查 OBF（bit0）再读，避免垃圾扫描码入缓冲。 */
    if (inb(0x64) & 0x01) {
        uint8_t sc = inb(0x60);
        uint16_t next_tail = (ps2_tail + 1) % PS2_BUFFER_SIZE;
        if (next_tail != ps2_head) {
            ps2_buffer[ps2_tail] = sc;
            ps2_tail = next_tail;
        }
    }
    pic_send_eoi(1);
}

extern void usb_keyboard_init(void);
extern bool usb_keyboard_has_char(void);
extern char usb_keyboard_get_char(void);
extern void usb_keyboard_poll(void);


/* 清掉启动残留的按键事件；PS/2 已被 BIOS 初始化，只需清缓冲 */
void keyboard_init(void) {
    ps2_head = ps2_tail = 0;
    shift_pressed = false;
    caps_lock = false;

    /* 清空 PS/2 控制器输出缓冲 */
    while (inb(0x64) & 0x01) (void)inb(0x60);

    vga_write("[PS/2 Keyboard] Initialized.\n");

    usb_keyboard_init();

    /* 屏蔽键盘 IRQ，纯轮询读取——避免 IRQ 与轮询竞争 */
    pic_mask_irq(1, true);
}


/* PS/2 或 USB 是否有按键待处理 */
bool keyboard_have_key(void) {
    if (ps2_head != ps2_tail) return true;
    return usb_keyboard_has_char();
}

/* 阻塞取键：先 PS/2 缓冲，再轮询 USB，都没有就 sti;hlt 等中断唤醒 */
char keyboard_get_char(void) {
    while (1) {
        if (ps2_head != ps2_tail) {
            uint8_t sc = ps2_buffer[ps2_head];
            ps2_head = (ps2_head + 1) % PS2_BUFFER_SIZE;

            /* 方向键：Up/Down 返回特殊码给 shell（命令历史） */
            if (!(sc & 0x80)) {
                switch (sc) {
                    case 0x48: return 0x01;   /* Up → 历史上翻 */
                    case 0x50: return 0x02;   /* Down → 历史下翻 */
                    case 0x4B: return '\b';  /* ← 当退格用：删光标前字符 */
                    case 0x4D: vga_move_right(); continue;
                    default: break;
                }
            }

            /* 转 ASCII（同时更新 shift/caps 状态） */
            char c = ps2_scancode_to_ascii(sc);
            if (c) return c;
            continue;

        } else {
            usb_keyboard_poll();
        }

        if (usb_keyboard_has_char()) {
            char c = usb_keyboard_get_char();
            if (c) return c;
        }

        /* ⚠️ 架构升级：等待按键改为中断休眠（sti;hlt）——旧实现纯轮询
         * 忙等 CPU 100%。IRQ1（键盘）把扫描码存入环形缓冲、IRQ12（鼠标）
         * 喂给鼠标解析器，都能唤醒 HLT。唤醒后检查缓冲。
         * 注意：本函数运行在 syscall 上下文（IF=0），sti 只在这段
         * 休眠窗口开中断；IRQ0 在该窗口触发时（from_user=0）现在也会
         * 调度（内核态抢占已启用）——被抢占时本任务挂起，恢复后继续
         * 休眠循环，语义不变（任务主动等待，抢占无副作用）。 */
        while (!(inb(0x64) & 0x21)) {   /* bit0=键盘数据 或 bit5=鼠标数据 */
            __asm__ volatile ("sti");
            __asm__ volatile ("hlt");
            __asm__ volatile ("cli");
            /* 也轮询 USB 键盘（非阻塞） */
            usb_keyboard_poll();
            if (usb_keyboard_has_char()) {
                char c = usb_keyboard_get_char();
                if (c) return c;
            }
        }
        /* 读空 PS/2 输出缓冲区的所有可用字节。
         *
         * ⚠️ 键盘和鼠标共用 0x60 数据口，分流顺序至关重要：
         *   状态寄存器 bit0（OBF）= 输出缓冲有数据，键盘鼠标都会置位！
         *   bit5（AUX）        = 这字节是鼠标数据。
         * 所以必须**先查 bit5（鼠标）再查 bit0（键盘）**——
         * 否则鼠标字节会被 bit0 抢先当成扫描码，产生乱字符。
         * （旧实现就犯了这个错：鼠标移动 → 屏幕上冒字符） */
        for (;;) {
            uint8_t st = inb(0x64);
            if (st & 0x20) {
                /* 鼠标数据 → 喂给鼠标协议解析器 */
                mouse_feed_byte(inb(0x60));
            } else if (st & 0x01) {
                /* 键盘数据 → 扫描码入环形缓冲 */
                uint8_t sc = inb(0x60);
                uint16_t next_tail = (ps2_tail + 1) % PS2_BUFFER_SIZE;
                if (next_tail != ps2_head) {
                    ps2_buffer[ps2_tail] = sc;
                    ps2_tail = next_tail;
                }
            } else {
                break;
            }
        }
    }
}
