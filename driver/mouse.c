/*
 * mouse.c - 统一鼠标驱动（PS/2 IRQ12 + USB HID 双后端）
 * PS/2 包：3 字节 [状态][X 位移][Y 位移]；USB 走 Boot Protocol。
 * USB 优先（支持热插拔），断开自动回退 PS/2。
 */

#include "../include/driver/mouse.h"
#include "../include/driver/pic.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/driver/usb.h"
#include "../include/driver/irqlock.h"
#include <stdint.h>
#include <stdbool.h>

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

static bool ps2_initialized = false;

/* 等 PS/2 设备 ACK（0xFA），超时约 10 万次轮询 */
static bool ps2_wait_ack(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) {
            if (inb(0x60) == 0xFA) return true;
        }
    }
    return false;
}

/* 发命令给鼠标：先 0x64 写 0xD4 指明目标，再 0x60 写命令 */
static bool ps2_send_command(uint8_t cmd) {
    outb(0x64, 0xD4);
    outb(0x60, cmd);
    return ps2_wait_ack();
}


/* 当前数据包的字节计数（凑齐 3 字节 = 一个完整数据包） */
static uint8_t ps2_pkt_cycle = 0;
static uint8_t ps2_pkt[3];


/* 位移 ÷ 灵敏度 = 光标位移（settings TUI 可调） */
#define MOUSE_SENSITIVITY_DEFAULT 8   /* 用户要求默认速度 8 */

/* 当前鼠标灵敏度（1~16） */
static int mouse_sensitivity = MOUSE_SENSITIVITY_DEFAULT;

/* 位移累加器（保留除法余数，保证小位移不丢失） */
static int ps2_acc_x = 0;
static int ps2_acc_y = 0;


static bool pointer_active = false;
static int pointer_row = 0, pointer_col = 0;
/* 指针下方的原始字符+属性（移动时恢复） */
static uint16_t pointer_under = 0x0F20;

/* 指针图案（CP437）：0xDB █ 实心、0xB2 ▓、0x10 ►、0x1E ▲ ……
 * ⚠️ 默认用箭头 0x1E（用户要求，最接近鼠标形状） */
#define MOUSE_POINTER_GLYPH_DEFAULT 0x1E

static uint8_t pointer_glyph = MOUSE_POINTER_GLYPH_DEFAULT;

static uint8_t invert_attr(uint8_t attr) {
    return (uint8_t)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
}

/* 由 vga.c 在一切文本输出前调用，防止输出字符和指针互相踩踏 */
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
/* 在 (row, col) 画指针（反显 + 可配置图案） */
static void pointer_draw(int row, int col) {
    uint32_t fl = irq_lock();
    /* ⚠️ 优化：位置未变则跳过重绘——高频移动时每个包都走
     * "恢复旧位置→画新位置"，同位置时是无效的擦除+重画（闪烁）。 */
    if (pointer_active && pointer_row == row && pointer_col == col) {
        irq_unlock(fl);
        return;
    }
    /* 先恢复旧位置 */
    if (pointer_active) {
        VGA_ADDR[pointer_row * VGA_WIDTH + pointer_col] = pointer_under;
    }
    pointer_row = row;
    pointer_col = col;

    uint32_t idx = row * VGA_WIDTH + col;
    pointer_under = VGA_ADDR[idx];
    uint8_t attr = (pointer_under >> 8) & 0xFF;
    VGA_ADDR[idx] = ((uint16_t)invert_attr(attr) << 8) | pointer_glyph;
    pointer_active = true;
    irq_unlock(fl);
}




/* 喂一个 PS/2 原始字节进协议解析器；轮询与 IRQ 两路共用，凑满 3 字节成一包 */
void mouse_feed_byte(uint8_t data) {
    ps2_pkt[ps2_pkt_cycle++] = data;
    if (ps2_pkt_cycle == 3) {
        if (ps2_pkt[0] & 0x08) {             /* bit3=1 表示包有效 */
            /* 位移量：bit 4/5 是 X/Y 符号位 */
            int dx = ps2_pkt[1];
            int dy = ps2_pkt[2];
            if (ps2_pkt[0] & 0x10) dx |= 0xFFFFFF00;  /* X 负值符号扩展 */
            if (ps2_pkt[0] & 0x20) dy |= 0xFFFFFF00;  /* Y 负值符号扩展 */

            /* 累加位置坐标（带灵敏度缩放：位移 ÷ mouse_sensitivity，
             * 余数进累加器，小位移不丢失） */
            ps2_acc_x += dx;
            ps2_acc_y += dy;
            mouse_x += ps2_acc_x / mouse_sensitivity;
            mouse_y -= ps2_acc_y / mouse_sensitivity;  /* ⚠️ PS/2 dy 正值=向上（数学坐标），屏幕 row 向上减小 */
            ps2_acc_x %= mouse_sensitivity;
            ps2_acc_y %= mouse_sensitivity;

            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= 80) mouse_x = 79;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= 25) mouse_y = 24;

            uint8_t btn = ps2_pkt[0] & 0x07;   /* 低 3 位：左/右/中键 */

            /* ⚠️ 修复（f173359 修正）：左键点击**不再**跳文本光标
             * （vga_set_cursor 已删除——误触会跳走打字位置）；
             * 指针绘制保留（用户需要看得见鼠标），反显方块跟随
             * 移动，但不影响文本光标与输入。 */
            mouse_btn = btn;

            pointer_draw(mouse_y, mouse_x);
        }
        ps2_pkt_cycle = 0;
    }
}

/* 消费 PS/2 缓冲里的鼠标字节（bit5=AUX），保证状态最新 */
void mouse_poll(void) {
    while (inb(0x64) & 0x20) {
        mouse_feed_byte(inb(0x60));
    }
}

/* IRQ12 中断处理：读一个字节交给公共解析器 */
void ps2_mouse_irq_handler(void) {
    /* ⚠️ 修复：读 0x60 前先查状态寄存器 AUX 位（bit5）。
     * 竞争场景：getchar 休眠唤醒后的 for(;;) 循环在 cli 下会把 PS/2
     * FIFO 读空（鼠标字节喂给解析器）；之后 IRQ12 才触发，handler
     * 此时 inb(0x60) 读到的是空 FIFO 的垃圾字节（0xFF 或旧值）——
     * 多出的字节会把 3 字节包错位，鼠标位置乱跳（“猎奇”体验）。
     * 查 bit5 再读：FIFO 已被清空则跳过，不产生垃圾包。 */
    if (inb(0x64) & 0x20) {
        mouse_feed_byte(inb(0x60));
    }
    pic_send_eoi(12);
}

/* 初始化序列：0xA8 启用设备 → 状态字节开鼠标中断 → 采样率/分辨率/缩放 → 0xF4 开报告 */
static bool ps2_init(void) {
    /* 启用 PS/2 鼠标设备（命令 0xA8 = Enable Auxiliary Device） */
    outb(0x64, 0xA8);

    /* 读取 Compaq 状态字节（0x20 = Read Command Byte），设置 bit 1 启用鼠标中断 */
    outb(0x64, 0x20);
    uint8_t status = inb(0x60);
    status |= 0x02;
    outb(0x64, 0x60);
    outb(0x60, status);

    ps2_send_command(0xF3);  /* 采样率 */
    ps2_send_command(100);   /* 100 次/秒 */
    ps2_send_command(0xE8);  /* 分辨率 */
    ps2_send_command(3);     /* 4 点/毫米 */
    ps2_send_command(0xE6);  /* 缩放 1:1 */

    /* 开数据报告——失败即鼠标不存在 */
    if (!ps2_send_command(0xF4)) {
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

static bool usb_mouse_present = false;
static usb_device_t *usb_dev = NULL;
static usb_endpoint_descriptor_t *usb_ep_in = NULL;
static usb_hc_t *usb_hc = NULL;
static uint8_t usb_report_buf[8];                    /* HID 报告缓冲区 */

/* USB HID Boot Protocol 报告结构 */
typedef struct {
    uint8_t buttons;    /* 按钮状态：bit0=左键, bit1=右键, bit2=中键 */
    int8_t  dx;         /* X 轴位移（有符号） */
    int8_t  dy;         /* Y 轴位移（有符号） */
} __attribute__((packed)) usb_mouse_report_t;

static void usb_process_report(uint8_t *data, int len) {
    if (len < 3) return;
    usb_mouse_report_t *rep = (usb_mouse_report_t*)data;
    int dx = rep->dx;
    int dy = rep->dy;
    /* 与 PS/2 路径一致的灵敏度缩放（余数进累加器） */
    ps2_acc_x += dx;
    ps2_acc_y += dy;
    mouse_x += ps2_acc_x / mouse_sensitivity;
    mouse_y += ps2_acc_y / mouse_sensitivity;  /* USB HID dy 正值=向下，屏幕 row 向下增大 */
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

/* 中断传输完成回调：处理报告后自动重提交，失败则回退 PS/2 */
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

/* 找 HID Boot Mouse：设配置、开 Boot Protocol、提交中断传输收报告 */
static bool usb_mouse_probe(void) {
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


/* 先 USB（支持热插拔）后 PS/2，都没有则标记不可用 */
void mouse_init(void) {
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
        return;
    }

    /* PS/2 后端初始化失败才走到这里：标记不可用 */

    mouse_available = false;
    vga_write("[Mouse] No mouse found.\n");
}


int mouse_get_sensitivity(void) {
    return mouse_sensitivity;
}

/* 设置灵敏度，限制 1~16 */
void mouse_set_sensitivity(int sens) {
    if (sens < 1) sens = 1;
    if (sens > 16) sens = 16;
    mouse_sensitivity = sens;
}

uint8_t mouse_get_pointer_glyph(void) {
    return pointer_glyph;
}

/* 设置指针图案（CP437 字形码） */
void mouse_set_pointer_glyph(uint8_t g) {
    pointer_glyph = g;
}


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
