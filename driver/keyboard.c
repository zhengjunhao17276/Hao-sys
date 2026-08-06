/**
 * =========================================================================
 * keyboard.c - 统一键盘驱动（PS/2 中断驱动 + USB 轮询）
 *
 * 该驱动同时支持 PS/2 键盘和 USB HID 键盘。PS/2 键盘使用 IRQ1
 * 中断方式——每次按键触发中断，将扫描码存入环形缓冲区。USB 键盘
 * 使用轮询方式——在 keyboard_get_char 中通过 usb_keyboard_poll()
 * 获取 HID 报告。
 *
 * PS/2 扫描码格式：
 *   按键按下触发 Make Code（< 0x80）
 *   按键释放触发 Break Code（= Make Code + 0x80）
 *   例如：'A' 按下 = 0x1E，'A' 释放 = 0x9E
 *
 * 修饰键（Shift/Caps Lock）的状态由全局变量维护，影响扫描码到
 * ASCII 的转换结果。
 *
 * 方向键已经被拦截用于 VGA 光标移动（up/down/left/right），
 * 不作为字符返回。
 * =========================================================================
 */

#include "../include/driver/keyboard.h"
#include "../include/driver/pic.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/driver/usb.h"
#include "../include/driver/mouse.h"
#include <stdint.h>
#include <stdbool.h>

/* ===================== 共享状态（与 USB 键盘共用） ===================== */
bool shift_pressed = false;      /* Shift 键是否被按下 */
bool caps_lock = false;          /* Caps Lock 是否锁定 */

/* ===================== PS/2 键盘环形缓冲区 ===================== */
#define PS2_BUFFER_SIZE 64       /* 环形缓冲区大小 */
static volatile uint8_t ps2_buffer[PS2_BUFFER_SIZE];  /* 扫描码缓冲区 */
static volatile uint16_t ps2_head = 0;   /* 读取位置（消费者索引） */
static volatile uint16_t ps2_tail = 0;   /* 写入位置（生产者索引） */

/**
 * PS/2 扫描码到 ASCII 的映射表（非 Shift 状态）
 *
 * 采用 C99 指定初始化器（designated initializer）语法，只初始化
 * 有意义的扫描码位置，其余默认为 0。这样可以一目了然地看到每个
 * 按键的映射关系。
 *
 * 扫描码来源：IBM PC/AT 键盘标准 Set 1。
 */
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

/**
 * PS/2 扫描码到 ASCII 的映射表（Shift 按下状态）
 * 与 normal_map 对应，但输出字母大写、数字变为符号。
 */
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

/* ===================== 字母大小写应用 ===================== */

/**
 * apply_caps_lock - 根据当前 shift 和 caps_lock 状态调整字母大小写
 *
 * 处理逻辑：
 *   Shift 和 Caps Lock 在字母上的关系是异或（XOR）：
 *   - Shift 按下但 Caps 关闭 → 大写
 *   - Shift 按下且 Caps 开启 → 小写（"反转"效果）
 *   - Shift 释放且 Caps 开启 → 大写
 *   - Shift 释放且 Caps 关闭 → 小写
 *
 * 非字母字符不受 Caps Lock 影响。
 */
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

/* ===================== PS/2 扫描码转换 ===================== */

/**
 * ps2_scancode_to_ascii - 将 PS/2 扫描码转换为 ASCII 字符
 * @sc: 从键盘读取的原始扫描码
 *
 * 返回值：
 *   0      → 修饰键（Shift/Ctrl/Alt/Caps）或按键释放事件
 *   非 0   → 转换后的 ASCII 字符
 *
 * 副作用：
 *   此函数会更新 shift_pressed 和 caps_lock 这两个全局状态变量。
 */
static char ps2_scancode_to_ascii(uint8_t sc) {
    if (sc & 0x80) {
        /* bit 7 = 1 → 按键释放事件 */
        uint8_t key = sc & 0x7F;
        if (key == 0x2A || key == 0x36) {
            shift_pressed = false;  /* Left/Right Shift 释放 */
        }
        return 0;
    }

    /* 按键按下事件 */
    if (sc == 0x2A || sc == 0x36) {
        shift_pressed = true;       /* Left/Right Shift 按下 */
        return 0;
    }

    if (sc == 0x3A) {               /* Caps Lock 按下 */
        caps_lock = !caps_lock;     /* 切换 Caps Lock 状态 */
        return 0;
    }

    /* 根据 Shift 状态选择映射表 */
    const char *map = shift_pressed ? shift_map : normal_map;
    char c = 0;
    if (sc < sizeof(normal_map)) {
        c = map[sc];
    }

    /* 应用 Caps Lock 逻辑 */
    if (c) c = apply_caps_lock(c);
    return c;
}

/* ===================== PS/2 中断处理 ===================== */

/**
 * keyboard_irq_handler - PS/2 键盘 IRQ1 中断处理程序
 *
 * 由 isr_asm.asm 中的 irq1_handler 调用。读取 PS/2 数据端口（0x60）
 * 获得扫描码，存入环形缓冲区（如果未满），然后发送 EOI。
 *
 * 注意：这个函数在中断上下文中执行，只能访问 volatile 变量。
 */
void keyboard_irq_handler(void) {
    /* ⚠️ 与 ps2_mouse_irq_handler 同理：getchar 的读空循环（cli 下）
     * 可能已把 FIFO 清空，此处 inb(0x60) 会读到空 FIFO 垃圾。
     * 先查 OBF（bit0）再读，避免垃圾扫描码入缓冲。 */
    if (inb(0x64) & 0x01) {
        uint8_t sc = inb(0x60);                   /* 从 PS/2 控制器读取扫描码 */
        uint16_t next_tail = (ps2_tail + 1) % PS2_BUFFER_SIZE;
        if (next_tail != ps2_head) {              /* 缓冲区未满 */
            ps2_buffer[ps2_tail] = sc;            /* 写入扫描码 */
            ps2_tail = next_tail;                 /* 更新尾指针 */
        }
    }
    pic_send_eoi(1);                              /* 发送 EOI 给 IRQ1 */
}

/* ===================== USB 键盘外部函数 ===================== */
extern void usb_keyboard_init(void);
extern bool usb_keyboard_has_char(void);
extern char usb_keyboard_get_char(void);
extern void usb_keyboard_poll(void);

/* ===================== 初始化 ===================== */

/**
 * keyboard_init - 初始化键盘驱动
 *
 * 清空 PS/2 控制器缓冲（可能残留了启动过程中的按键事件），
 * 然后初始化 USB 键盘子系统（如果存在）。
 * PS/2 键盘已经在 BIOS 阶段完成初始化，只需清空缓冲即可。
 */
void keyboard_init(void) {
    ps2_head = ps2_tail = 0;
    shift_pressed = false;
    caps_lock = false;

    /* 清空 PS/2 控制器输出缓冲 */
    while (inb(0x64) & 0x01) (void)inb(0x60);

    vga_write("[PS/2 Keyboard] Initialized.\n");

    /* 初始化 USB 键盘（如果存在 USB 设备） */
    usb_keyboard_init();

    /* 屏蔽键盘 IRQ，使用纯轮询方式读取键盘（避免 IRQ 与轮询竞争） */
    pic_mask_irq(1, true);
}

/* ===================== 字符读取 ===================== */

/**
 * keyboard_have_key - 检查是否有按键等待处理
 *
 * 同时检查 PS/2 缓冲区和 USB 键盘缓冲区。
 */
bool keyboard_have_key(void) {
    if (ps2_head != ps2_tail) return true;
    return usb_keyboard_has_char();
}

/**
 * keyboard_get_char - 阻塞式获取一个键盘输入的 ASCII 字符
 *
 * 获取策略：
 *   1. 优先从 PS/2 环形缓冲区读取扫描码
 *   2. 非字符的扫描码（方向键）直接处理为光标移动
 *   3. 修饰键（Shift/Caps）更新状态但不返回字符
 *   4. 如果没有 PS/2 数据，轮询 USB 键盘
 *   5. 如果都没有数据，执行 HLT 等待中断唤醒
 */
char keyboard_get_char(void) {
    while (1) {
        /* ===== 处理 PS/2 缓冲区 ===== */
        if (ps2_head != ps2_tail) {
            uint8_t sc = ps2_buffer[ps2_head];
            ps2_head = (ps2_head + 1) % PS2_BUFFER_SIZE;

            /* 处理方向键：Up/Down 返回特殊码给 shell（命令历史用），
             * Left/Right 仍直接移动 VGA 光标（编辑当前行用） */
            if (!(sc & 0x80)) { /* 按下事件 */
                switch (sc) {
                    case 0x48: return 0x01;   /* Up   → 特殊码 0x01（历史上翻） */
                    case 0x50: return 0x02;   /* Down → 特殊码 0x02（历史下翻） */
                    case 0x4B: vga_move_left();  continue;
                    case 0x4D: vga_move_right(); continue;
                    default: break;
                }
            }

            /* 转换为 ASCII（同时更新 shift/caps 状态） */
            char c = ps2_scancode_to_ascii(sc);
            if (c) return c;       /* 返回有效的 ASCII 字符 */
            continue;              /* 修饰键等不产生字符的事件 */

        } else {
            /* PS/2 缓冲区为空时，主动轮询 USB 键盘获取报告 */
            usb_keyboard_poll();
        }

        /* ===== 检查 USB 键盘缓冲区 ===== */
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
                break;   /* 输出缓冲已空 */
            }
        }
    }
}
