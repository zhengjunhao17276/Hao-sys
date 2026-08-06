/**
 * =========================================================================
 * mouse.c - 统一鼠标驱动（PS/2 + USB 双后端）
 *
 * 本驱动实现了两个后端：
 *   - PS/2 后端：使用 IRQ12 中断方式，每次中断读取 3 字节的鼠标数据包
 *   - USB 后端：通过中断传输获取 HID 鼠标报告
 *
 * 鼠标数据包解析（PS/2 标准 3 字节格式）：
 *   字节 0: [Y 溢出] [X 溢出] [Y 符号] [X 符号] [Alway 1] [中键] [右键] [左键]
 *   字节 1: X 位移量（有符号，受 X 符号位影响）
 *   字节 2: Y 位移量（有符号，受 Y 符号位影响）
 *
 * USB HID 鼠标报告格式（简化的 Boot Protocol）：
 *   字节 0: 按钮状态
 *   字节 1: X 位移（有符号 int8）
 *   字节 2: Y 位移（有符号 int8）
 *
 * 后端自动切换：
 *   USB 鼠标优先（支持热插拔），回退到 PS/2。如果 USB 断开，
 *   PS/2 被选为后备方案。
 * =========================================================================
 */

#include "../include/driver/mouse.h"
#include "../include/driver/pic.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/driver/usb.h"
#include "../include/driver/irqlock.h"
#include <stdint.h>
#include <stdbool.h>

/* ==================== 共享状态 ==================== */
static volatile int mouse_x = 0, mouse_y = 0;   /* 鼠标位置（累计模式） */
static volatile uint8_t mouse_btn = 0;           /* 按钮状态位 */
static bool mouse_available = false;             /* 鼠标是否可用 */

/* 当前使用的后端 */
typedef enum {
    BACKEND_NONE,
    BACKEND_PS2,
    BACKEND_USB
} backend_t;
static backend_t current_backend = BACKEND_NONE;

/* ==================== PS/2 后端 ==================== */
static bool ps2_initialized = false;

/**
 * ps2_wait_ack - 等待 PS/2 设备返回 ACK（0xFA）
 * 超时约 10 万次轮询。返回 true 表示收到 ACK。
 */
static bool ps2_wait_ack(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) {         /* 输出缓冲满 */
            if (inb(0x60) == 0xFA) return true;  /* 0xFA = ACK */
        }
    }
    return false;  /* 超时 */
}

/**
 * ps2_send_command - 向 PS/2 鼠标发送命令
 * @cmd: 命令字节
 *
 * 先通过端口 0x64 写 0xD4（表示下一个字节发给鼠标），
 * 然后通过端口 0x60 写命令字节。
 */
static bool ps2_send_command(uint8_t cmd) {
    outb(0x64, 0xD4);         /* 0xD4 = 下一个端口 0x60 的写入目标为鼠标 */
    outb(0x60, cmd);           /* 发送命令到鼠标 */
    return ps2_wait_ack();
}

/* ===================== PS/2 鼠标协议解析 ===================== */

/** 当前数据包的字节计数（凑齐 3 字节 = 一个完整数据包） */
static uint8_t ps2_pkt_cycle = 0;
/** 3 字节数据包缓存 */
static uint8_t ps2_pkt[3];

/* ===================== 灵敏度控制 ===================== */

/**
 * MOUSE_SENSITIVITY_DEFAULT - 默认鼠标灵敏度
 *
 * 实际位移量（dx/dy）除以当前灵敏度才是光标位移：
 *   1 = 原始 1:1（最灵敏）
 *   2 = 半速
 *   4 = 四分之一速
 * 运行时可通过 mouse_set_sensitivity() 调整（settings TUI 用）。
 */
#define MOUSE_SENSITIVITY_DEFAULT 2

/** 当前鼠标灵敏度（1~16） */
static int mouse_sensitivity = MOUSE_SENSITIVITY_DEFAULT;

/** 位移累加器（保留除法余数，保证小位移不丢失） */
static int ps2_acc_x = 0;
static int ps2_acc_y = 0;

/* ===================== 鼠标指针渲染（与文本光标分离） ===================== */

/** 指针当前是否可见 */
static bool pointer_active = false;
/** 指针所在位置 */
static int pointer_row = 0, pointer_col = 0;
/** 指针下方的原始字符+属性（移动时恢复） */
static uint16_t pointer_under = 0x0F20;
/** 上一包的按钮状态（用于检测按下沿 = 点击） */

/**
 * MOUSE_POINTER_GLYPH_DEFAULT - 默认鼠标指针图案（CP437 字形）
 *   0xDB █ 实心方块（默认，最醒目）
 *   0xB2 ▓ 深阴影    0xB1 ▒ 中阴影   0xB0 ░ 浅阴影
 *   0x10 ► 右箭头    0x11 ◄ 左箭头   0x1E ▲ 上箭头   0x1F ▼ 下箭头
 * 运行时可通过 mouse_set_pointer_glyph() 调整（settings TUI 用）。
 */
#define MOUSE_POINTER_GLYPH_DEFAULT 0xDB

/** 当前鼠标指针图案（0~255） */
static uint8_t pointer_glyph = MOUSE_POINTER_GLYPH_DEFAULT;

/** 属性反转：交换前景/背景 nibble（反显效果） */
static uint8_t invert_attr(uint8_t attr) {
    return (uint8_t)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
}

/**
 * mouse_pointer_erase - 擦除鼠标指针（恢复被覆盖的字符）
 *
 * 由 vga.c 在一切文本输出前调用，防止输出字符和指针互相踩踏。
 */
void mouse_pointer_erase(void) {
    /* ⚠️ 内核态抢占：指针状态与显存写入必须与 vga 输出（持锁）互斥。
     * 本函数可能从 vga 锁内（putchar_core）或 IRQ12 上下文调用，
     * 嵌套锁安全（pushfl/popfl 配对）。 */
    uint32_t fl = irq_lock();
    if (!pointer_active) { irq_unlock(fl); return; }
    VGA_ADDR[pointer_row * VGA_WIDTH + pointer_col] = pointer_under;
    pointer_active = false;
    irq_unlock(fl);
}
/** pointer_draw - 在指定位置绘制鼠标指针（固定图案 + 反显属性） */
static void pointer_draw(int row, int col) {
    uint32_t fl = irq_lock();
    /* 先恢复旧位置 */
    if (pointer_active) {
        VGA_ADDR[pointer_row * VGA_WIDTH + pointer_col] = pointer_under;
    }
    pointer_row = row;
    pointer_col = col;

    uint32_t idx = row * VGA_WIDTH + col;
    pointer_under = VGA_ADDR[idx];
    uint8_t attr = (pointer_under >> 8) & 0xFF;
    /* 用可配置图案渲染指针（默认实心方块） */
    VGA_ADDR[idx] = ((uint16_t)invert_attr(attr) << 8) | pointer_glyph;
    pointer_active = true;
    irq_unlock(fl);
}




/**
 * mouse_feed_byte - 向 PS/2 鼠标协议解析器喂一个字节
 * @data: 从 PS/2 数据端口（0x60）读出的原始字节
 *
 * 数据包格式（标准 PS/2 鼠标）：每 3 个字节一组：
 *   字节 0: 状态（按钮 + 符号位 + 溢出位），bit3 通常为 1
 *   字节 1: X 位移（有符号）
 *   字节 2: Y 位移（有符号）
 *
 * 轮询路径（keyboard_get_char 的 PS/2 排空循环）和 IRQ 路径
 * （ps2_mouse_irq_handler）都通过这个函数解析，保证两路不重复实现。
 */
void mouse_feed_byte(uint8_t data) {
    ps2_pkt[ps2_pkt_cycle++] = data;
    if (ps2_pkt_cycle == 3) {                /* 凑齐完整 3 字节包 */
        if (ps2_pkt[0] & 0x08) {             /* bit 3 通常为 1，表示数据包有效 */
            /* 解析位移量——字节 0 的 bit 4/5 是 X/Y 符号位 */
            int dx = ps2_pkt[1];
            int dy = ps2_pkt[2];
            if (ps2_pkt[0] & 0x10) dx |= 0xFFFFFF00;  /* X 符号扩展（负数） */
            if (ps2_pkt[0] & 0x20) dy |= 0xFFFFFF00;  /* Y 符号扩展（负数） */

            /* 累加位置坐标（带灵敏度缩放：位移 ÷ mouse_sensitivity，
             * 余数进累加器，小位移不丢失） */
            ps2_acc_x += dx;
            ps2_acc_y += dy;
            mouse_x += ps2_acc_x / mouse_sensitivity;
            mouse_y -= ps2_acc_y / mouse_sensitivity;
            ps2_acc_x %= mouse_sensitivity;
            ps2_acc_y %= mouse_sensitivity;

            /* 裁剪到 VGA 显示范围（80×25） */
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= 80) mouse_x = 79;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= 25) mouse_y = 24;

            /* 按钮状态（低 3 位：左键/右键/中键） */
            uint8_t btn = ps2_pkt[0] & 0x07;

            /* ⚠️ 修复（f173359 修正）：左键点击**不再**跳文本光标
             * （vga_set_cursor 已删除——误触会跳走打字位置）；
             * 指针绘制保留（用户需要看得见鼠标），反显方块跟随
             * 移动，但不影响文本光标与输入。 */
            mouse_btn = btn;

            /* 指针自由移动：反显渲染跟随鼠标位置 */
            pointer_draw(mouse_y, mouse_x);
        }
        ps2_pkt_cycle = 0;   /* 重置，准备下一个数据包 */
    }
}

/**
 * mouse_poll - 轮询式消费 PS/2 鼠标数据
 *
 * 检查 PS/2 状态寄存器（0x64）的 bit 5（输出缓冲含鼠标数据），
 * 把所有待处理的鼠标字节喂给协议解析器。
 * 纯轮询模式下由 sys_getmouse 调用，保证鼠标状态是新鲜的。
 */
void mouse_poll(void) {
    while (inb(0x64) & 0x20) {
        mouse_feed_byte(inb(0x60));
    }
}

/**
 * ps2_mouse_irq_handler - PS/2 鼠标 IRQ12 中断处理程序
 *
 * 读取一个字节交给公共解析器。当前系统采用纯轮询模式
 * （IRQ12 已被 mouse_init 屏蔽），此函数保留供将来开启中断时使用。
 */
void ps2_mouse_irq_handler(void) {
    mouse_feed_byte(inb(0x60));
    pic_send_eoi(12);  /* 发送 EOI 给 IRQ12 */
}

/**
 * ps2_init - 初始化 PS/2 鼠标
 *
 * 初始化序列：
 *   1. 启用鼠标（端口 0x64, 命令 0xA8）
 *   2. 设置 Compaq 状态字节（0x64/0x60）
 *   3. 设置采样率（0xF3）为 100Hz
 *   4. 设置分辨率（0xE8）为 4 点/mm
 *   5. 设置缩放（0xE6）为 1:1
 *   6. 启用数据报告（0xF4）
 */
static bool ps2_init(void) {
    /* 启用 PS/2 鼠标设备（命令 0xA8 = Enable Auxiliary Device） */
    outb(0x64, 0xA8);

    /* 读取 Compaq 状态字节（0x20 = Read Command Byte），设置 bit 1 启用鼠标中断 */
    outb(0x64, 0x20);
    uint8_t status = inb(0x60);
    status |= 0x02;       /* 启用鼠标中断 */
    outb(0x64, 0x60);     /* 写命令字节 */
    outb(0x60, status);

    /* 配置鼠标参数 */
    ps2_send_command(0xF3);  /* 设置采样率 */
    ps2_send_command(100);   /* 100 次/秒 */
    ps2_send_command(0xE8);  /* 设置分辨率 */
    ps2_send_command(3);     /* 4 点/毫米 */
    ps2_send_command(0xE6);  /* 缩放 1:1 */

    /* 启用数据报告——如果失败则说明鼠标不存在 */
    if (!ps2_send_command(0xF4)) {  /* 0xF4 = Enable Data Reporting */
        vga_write("[PS/2 Mouse] Enable data report failed.\n");
        return false;
    }
    ps2_initialized = true;
    vga_write("[PS/2 Mouse] Initialized.\n");
    return true;
}

static void ps2_get_packet(int *x, int *y, int *buttons) {
    /* ⚠️ 修复：保存/恢复 EFLAGS 而非 cli/sti——本函数可能在 syscall
     * 上下文（IF=0）被调用，直接 sti 会提前开中断，让 IRQ 打断内核
     * 代码。pushfl/popfl 精确还原原中断状态。 */
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
    mouse_poll();                                     /* 先消费待处理的鼠标数据 */
    *x = mouse_x;
    *y = mouse_y;
    *buttons = mouse_btn;
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags));
}

/* ==================== USB 后端 ==================== */
static bool usb_mouse_present = false;
static usb_device_t *usb_dev = NULL;
static usb_endpoint_descriptor_t *usb_ep_in = NULL;
static usb_hc_t *usb_hc = NULL;
static uint8_t usb_report_buf[8];                    /* HID 报告缓冲区 */

/**
 * USB 鼠标报告结构（Boot Protocol 格式）
 */
typedef struct {
    uint8_t buttons;    /* 按钮状态：bit0=左键, bit1=右键, bit2=中键 */
    int8_t  dx;         /* X 轴位移（有符号） */
    int8_t  dy;         /* Y 轴位移（有符号） */
} __attribute__((packed)) usb_mouse_report_t;

/**
 * usb_process_report - 处理 USB HID 鼠标报告
 */
static void usb_process_report(uint8_t *data, int len) {
    if (len < 3) return;
    usb_mouse_report_t *rep = (usb_mouse_report_t*)data;
    int dx = rep->dx;
    int dy = rep->dy;
    /* 与 PS/2 路径一致的灵敏度缩放（余数进累加器） */
    ps2_acc_x += dx;
    ps2_acc_y += dy;
    mouse_x += ps2_acc_x / mouse_sensitivity;
    mouse_y -= ps2_acc_y / mouse_sensitivity;
    ps2_acc_x %= mouse_sensitivity;
    ps2_acc_y %= mouse_sensitivity;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= 80) mouse_x = 79;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= 25) mouse_y = 24;
    mouse_btn = rep->buttons & 0x07;
    /* 指针跟随（与 PS/2 路径一致） */
    pointer_draw(mouse_y, mouse_x);
}

/**
 * usb_mouse_urb_callback - USB 鼠标中断传输完成回调
 *
 * 处理完当前报告后，自动重新提交下一个中断传输以持续接收数据。
 * 如果重新提交失败（如设备断开），切换到 PS/2 后端。
 */
static void usb_mouse_urb_callback(usb_urb_t *urb) {
    if (urb->length < 3) {
        usb_mouse_present = false;
        vga_write("[USB Mouse] Disconnected (invalid report).\n");
        if (current_backend == BACKEND_USB) {
            if (ps2_initialized) {
                current_backend = BACKEND_PS2;
                mouse_available = true;
                vga_write("[Mouse] Switched to PS/2 backend.\n");
            } else {
                mouse_available = false;
                current_backend = BACKEND_NONE;
            }
        }
        return;
    }

    usb_process_report((uint8_t*)urb->buffer, urb->length);

    /* 自动重新提交中断传输，持续接收后续报告 */
    if (usb_mouse_present && usb_dev && usb_ep_in) {
        usb_urb_t new_urb = {
            .type = USB_URB_INTERRUPT,
            .dev_addr = usb_dev->address,
            .endpoint = usb_ep_in->bEndpointAddress & 0x0F,
            .direction = 1,
            .length = usb_ep_in->wMaxPacketSize,
            .buffer = usb_report_buf,
            .callback = usb_mouse_urb_callback,
        };
        int ret = usb_submit_urb(usb_hc, &new_urb);
        if (ret < 0) {
            usb_mouse_present = false;
            vga_write("[USB Mouse] Resubmit failed, removed.\n");
            if (current_backend == BACKEND_USB) {
                if (ps2_initialized) {
                    current_backend = BACKEND_PS2;
                    mouse_available = true;
                    vga_write("[Mouse] Switched to PS/2 backend.\n");
                } else {
                    mouse_available = false;
                    current_backend = BACKEND_NONE;
                }
            }
        }
    }
}

extern bool find_hid_interface(usb_hc_t *hc, uint8_t dev_addr,
                               const uint8_t *config_data, uint16_t total_len,
                               usb_interface_descriptor_t **out_iface,
                               usb_endpoint_descriptor_t **out_ep_in,
                               usb_endpoint_descriptor_t **out_ep_out);

/**
 * usb_mouse_probe - 探测 USB 鼠标设备
 *
 * 遍历 USB 设备列表，寻找 HID 设备。如果是复合设备（bDeviceClass=0x00），
 * 读取配置描述符并查找 HID 接口。找到后设置配置、启用 HID Boot Protocol、
 * 提交中断传输开始接收报告。
 */
static bool usb_mouse_probe(void) {
    vga_write("[USB Mouse] Probing...\n");
    usb_device_t *dev = usb_get_device_list();
    while (dev) {
        bool is_hid = false;

        /* 设备级别类为 0x03（HID 专用设备）或 0x00（接口定义类），
         * 都要读配置描述符确认接口协议：只有协议 2（Boot Mouse）
         * 才是鼠标——否则会把 USB 键盘（协议 1）误认成鼠标抢走。 */
        if (dev->dev_desc.bDeviceClass == 0x03 || dev->dev_desc.bDeviceClass == 0x00) {
            uint8_t config_buf[256];
            int ret = usb_control_transfer(dev->hc, dev->address,
                                           0x80, USB_REQ_GET_DESCRIPTOR,
                                           (USB_DESC_CONFIGURATION << 8) | 0,
                                           0, config_buf, sizeof(config_buf));
            if (ret < 0) { dev = dev->next; continue; }

            usb_config_descriptor_t *cfg = (usb_config_descriptor_t*)config_buf;
            uint16_t total = cfg->wTotalLength;
            if (total > sizeof(config_buf)) total = sizeof(config_buf);

            usb_interface_descriptor_t *iface = NULL;
            usb_endpoint_descriptor_t *ep_in = NULL, *ep_out = NULL;
            if (find_hid_interface(dev->hc, dev->address,
                                   config_buf, total, &iface, &ep_in, &ep_out) &&
                iface && iface->bInterfaceProtocol == 2) {   /* 2 = Boot Mouse */
                is_hid = true;
                usb_ep_in = ep_in;

                /* 设置配置*/
                ret = usb_control_transfer(dev->hc, dev->address,
                                           0x00, USB_REQ_SET_CONFIGURATION,
                                           cfg->bConfigurationValue, 0, NULL, 0);
                if (ret < 0) {
                    vga_write("[USB Mouse] Set config failed.\n");
                    is_hid = false;
                }
            }
        }

        if (is_hid) {
            usb_dev = dev;
            usb_hc = dev->hc;
            vga_write("[USB Mouse] Found VID=");
            vga_write_hex(dev->dev_desc.idVendor);
            vga_write(" PID=");
            vga_write_hex(dev->dev_desc.idProduct);
            vga_write("\n");

            /* 启用 HID Boot Protocol（0x0B = SET_PROTOCOL, 0x00 = Boot） */
            usb_control_transfer(dev->hc, dev->address,
                                 0x21, 0x0B, 0x0000, 0, NULL, 0);
            /* 设置空闲率（0x0A = SET_IDLE, 0x0000 = 无限期等待报告） */
            usb_control_transfer(dev->hc, dev->address,
                                 0x21, 0x0A, 0x0000, 0, NULL, 0);

            if (usb_ep_in) {
                /* 提交中断传输——USB 鼠标开始持续报告位置变化 */
                usb_urb_t urb = {
                    .type = USB_URB_INTERRUPT,
                    .dev_addr = dev->address,
                    .endpoint = usb_ep_in->bEndpointAddress & 0x0F,
                    .direction = 1,
                    .length = usb_ep_in->wMaxPacketSize,
                    .buffer = usb_report_buf,
                    .callback = usb_mouse_urb_callback,
                };
                int ret = usb_submit_urb(usb_hc, &urb);
                if (ret == 0) {
                    usb_mouse_present = true;
                    vga_write("[USB Mouse] Interrupt transfer started.\n");
                    return true;
                } else {
                    vga_write("[USB Mouse] URB submit failed. Error code: ");
                    vga_write_hex((uint32_t)ret);
                    vga_write("\n");
                }
            } else {
                vga_write("[USB Mouse] No IN endpoint.\n");
            }
        }
        dev = dev->next;
    }
    vga_write("[USB Mouse] No device found.\n");
    return false;
}

static void usb_get_packet(int *x, int *y, int *buttons) {
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
    if (!usb_mouse_present) {
        *x = 0; *y = 0; *buttons = 0;
    } else {
        *x = mouse_x;
        *y = mouse_y;
        *buttons = mouse_btn;
    }
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags));
}

/* ==================== 统一初始化 ==================== */

/**
 * mouse_init - 初始化统一鼠标驱动
 *
 * 探测顺序：
 *   1. 先尝试 USB 鼠标（优先级高，支持热插拔）
 *   2. 如果 USB 不存在，回退到 PS/2 鼠标
 *   3. 如果 PS/2 也不存在，标记为不可用
 */
void mouse_init(void) {
    vga_write("[Mouse] Initializing unified driver...\n");

    if (usb_mouse_probe()) {
        current_backend = BACKEND_USB;
        mouse_available = true;
        vga_write("[Mouse] Using USB backend.\n");
        ps2_init();     /* 也初始化 PS/2 作为后备 */
        return;
    }

    if (ps2_init()) {
        current_backend = BACKEND_PS2;
        mouse_available = true;
        /* ⚠️ 架构升级：保持 IRQ12 中断模式——键盘等待从纯轮询改为
         * sti;hlt 休眠后，鼠标数据由 IRQ12 中断喂给解析器（
         * ps2_mouse_irq_handler），不再需要键盘排空循环代劳。
         * 键盘 IRQ1 与鼠标 IRQ12 由 PS/2 控制器按来源区分，互不误读。 */
        vga_write("[Mouse] Using PS/2 backend (interrupt-driven).\n");
        return;
    }

    mouse_available = false;
    vga_write("[Mouse] No mouse found.\n");
}

/* ==================== 灵敏度读写（settings TUI 用） ==================== */

/**
 * mouse_get_sensitivity - 读取当前鼠标灵敏度
 * @return 灵敏度（1~16）
 */
int mouse_get_sensitivity(void) {
    return mouse_sensitivity;
}

/**
 * mouse_set_sensitivity - 设置鼠标灵敏度
 * @sens: 目标灵敏度，自动限制在 1~16 范围内
 */
void mouse_set_sensitivity(int sens) {
    if (sens < 1) sens = 1;
    if (sens > 16) sens = 16;
    mouse_sensitivity = sens;
}

/**
 * mouse_get_pointer_glyph - 读取当前鼠标指针图案
 * @return 图案字符（CP437 字形码，0~255）
 */
uint8_t mouse_get_pointer_glyph(void) {
    return pointer_glyph;
}

/**
 * mouse_set_pointer_glyph - 设置鼠标指针图案
 * @g: 图案字符（CP437 字形码），如 0xDB=█、0x10=►
 */
void mouse_set_pointer_glyph(uint8_t g) {
    pointer_glyph = g;
}

/* ==================== 统一获取数据 ==================== */

void mouse_get_packet(int *x, int *y, int *buttons) {
    if (!mouse_available) {
        *x = 0; *y = 0; *buttons = 0;
        return;
    }
    switch (current_backend) {
        case BACKEND_PS2: ps2_get_packet(x, y, buttons); break;
        case BACKEND_USB: usb_get_packet(x, y, buttons); break;
        default: *x = *y = *buttons = 0; break;
    }
}
