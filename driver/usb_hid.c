/*
 * usb_hid.c - USB HID 驱动（键盘 Boot Protocol 完整驱动 + HID 探测）
 * 键盘：8 字节 Boot 报告，沿检测入环形缓冲，中断传输轮询收数。
 */

#include "../include/driver/usb.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include <stddef.h>
#include <stdbool.h>

/* find_hid_interface 已在 usb_controller.c 中定义，此处不再重复 */

/* ---- USB 键盘（Boot Protocol 完整驱动） ---- */

/* HID 类请求 */
#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_REQ_SET_IDLE     0x0A

/* 键盘字符环形缓冲 */
#define USB_KB_BUF_SIZE 32
static volatile char usb_kb_buf[USB_KB_BUF_SIZE];
static volatile int usb_kb_head = 0, usb_kb_tail = 0;

static bool usb_kb_present = false;
static usb_hc_t *usb_kb_hc = NULL;
static uint8_t usb_kb_addr = 0;
static uint8_t usb_kb_ep = 0;
static uint8_t usb_kb_iface = 0;
static uint8_t usb_kb_report[8];       /* 中断传输缓冲区（当前报告） */
static uint8_t usb_kb_prev[8];         /* 上一份报告（按键沿检测） */
static bool usb_kb_first_report = true;

static void usb_kb_push(char c) {
    int next = (usb_kb_tail + 1) % USB_KB_BUF_SIZE;
    if (next != usb_kb_head) {
        usb_kb_buf[usb_kb_tail] = c;
        usb_kb_tail = next;
    }
}

/* HID 用法码 → ASCII（0=无映射：Ctrl/Alt/未知键） */
static char usb_kb_map(uint8_t usage, bool shift) {
    /* 字母 a-z（0x04-0x1D） */
    if (usage >= 0x04 && usage <= 0x1D)
        return (char)((shift ? 'A' : 'a') + (usage - 0x04));
    /* 数字行 1-0（0x1E-0x27） */
    if (usage >= 0x1E && usage <= 0x27) {
        static const char n[] = "1234567890";
        static const char s[] = "!@#$%^&*()";
        return shift ? s[usage - 0x1E] : n[usage - 0x1E];
    }
    switch (usage) {
        case 0x28: return '\n';        /* Enter */
        case 0x29: return 0x1B;        /* Esc */
        case 0x2A: return '\b';        /* Backspace */
        case 0x2B: return '\t';        /* Tab */
        case 0x2C: return ' ';         /* Space */
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x52: return 0x01;        /* Up → 命令历史 */
        case 0x51: return 0x02;        /* Down → 命令历史 */
        default: return 0;             /* Ctrl/Alt/未知键 */
    }
}

/* 报告里是否含某用法码 */
static bool usb_kb_has_key(const uint8_t *r, uint8_t usage) {
    for (int i = 2; i < 8; i++)
        if (r[i] == usage) return true;
    return false;
}

/* 中断传输回调。报告 8 字节：byte0 修饰键（bit1/5=Shift）、
 * byte2-7 最多 6 个用法码。按键是电平状态：新旧报告比对出"按下"，
 * 解析完重提交维持轮询。 */
static void usb_kb_callback(usb_urb_t *urb) {
    uint8_t *r = (uint8_t*)urb->buffer;
    bool shift = (r[0] & 0x02) || (r[0] & 0x20);   /* LShift / RShift */

    if (!usb_kb_first_report) {
        /* 按键沿检测 */
        for (int i = 2; i < 8; i++) {
            uint8_t k = r[i];
            if (k != 0 && !usb_kb_has_key(usb_kb_prev, k)) {
                char c = usb_kb_map(k, shift);
                if (c) usb_kb_push(c);
            }
        }
    } else {
        usb_kb_first_report = false;   /* 丢弃首份报告（可能是状态快照） */
    }
    for (int i = 0; i < 8; i++) usb_kb_prev[i] = r[i];

    /* 重新提交以持续轮询（uhci_interrupt_transfer 会复用 TD） */
    if (usb_kb_present && usb_kb_hc) {
        usb_urb_t nu = {
            .type = USB_URB_INTERRUPT,
            .dev_addr = usb_kb_addr,
            .endpoint = usb_kb_ep,
            .direction = 1,
            .length = 8,
            .buffer = usb_kb_report,
            .callback = usb_kb_callback,
        };
        usb_submit_urb(usb_kb_hc, &nu);
    }
}

void usb_keyboard_init(void) {
    vga_write("[USB Keyboard] Probing...\n");

    usb_device_t *dev = usb_get_device_list();
    while (dev && !usb_kb_present) {
        uint8_t buffer[256];
        int ret = usb_control_transfer(dev->hc, dev->address,
                                       0x80, USB_REQ_GET_DESCRIPTOR,
                                       (USB_DESC_CONFIGURATION << 8) | 0,
                                       0, buffer, sizeof(buffer));
        if (ret < 0) { dev = dev->next; continue; }
        usb_config_descriptor_t *cfg = (usb_config_descriptor_t*)buffer;
        uint16_t total = cfg->wTotalLength;
        if (total > sizeof(buffer)) total = sizeof(buffer);

        usb_interface_descriptor_t *iface = NULL;
        usb_endpoint_descriptor_t *ep_in = NULL, *ep_out = NULL;
        if (find_hid_interface(dev->hc, dev->address, buffer, total,
                               &iface, &ep_in, &ep_out) &&
            iface && ep_in && iface->bInterfaceProtocol == 1) {  /* 1 = 键盘 */
            usb_kb_hc = dev->hc;
            usb_kb_addr = dev->address;
            usb_kb_ep = ep_in->bEndpointAddress & 0x0F;
            usb_kb_iface = iface->bInterfaceNumber;
            usb_kb_present = true;

            vga_write("[USB Keyboard] Found (VID=");
            vga_write_hex(dev->dev_desc.idVendor);
            vga_write(" PID=");
            vga_write_hex(dev->dev_desc.idProduct);
            vga_write(")\n");

            /* 设置配置 + Boot Protocol + 无限轮询间隔 */
            usb_control_transfer(dev->hc, dev->address, 0x00,
                                 USB_REQ_SET_CONFIGURATION,
                                 cfg->bConfigurationValue, 0, NULL, 0);
            usb_control_transfer(dev->hc, dev->address, 0x21,
                                 HID_REQ_SET_PROTOCOL, 0x0000, usb_kb_iface, NULL, 0);
            usb_control_transfer(dev->hc, dev->address, 0x21,
                                 HID_REQ_SET_IDLE, 0x0000, usb_kb_iface, NULL, 0);

            /* 提交首个中断传输 */
            usb_urb_t urb = {
                .type = USB_URB_INTERRUPT,
                .dev_addr = usb_kb_addr,
                .endpoint = usb_kb_ep,
                .direction = 1,
                .length = 8,
                .buffer = usb_kb_report,
                .callback = usb_kb_callback,
            };
            if (usb_submit_urb(usb_kb_hc, &urb) == 0) {
                vga_write("[USB Keyboard] Interrupt transfer started.\n");
            } else {
                vga_write("[USB Keyboard] URB submit failed.\n");
                usb_kb_present = false;
            }
        }
        dev = dev->next;
    }

    if (!usb_kb_present)
        vga_write("[USB Keyboard] No USB keyboard (PS/2 in use).\n");
}

bool usb_keyboard_has_char(void) {
    return usb_kb_head != usb_kb_tail;
}

char usb_keyboard_get_char(void) {
    if (usb_kb_head == usb_kb_tail) return 0;
    char c = usb_kb_buf[usb_kb_head];
    usb_kb_head = (usb_kb_head + 1) % USB_KB_BUF_SIZE;
    return c;
}

void usb_keyboard_poll(void) {
    /* 处理 UHCI 中断传输完成（回调里解析报告入缓冲） */
    usb_poll();
}

/* ---- USB HID 初始化 ---- */

/* 扫设备列表认领 HID 设备（类码 0x03，或 0x00 时查接口描述符），
 * 找到即设置配置。当前只打印信息，不起中断传输。 */
void usb_hid_init(void) {
    vga_write("[USB HID] Scanning for HID devices...\n");

    usb_device_t *dev = usb_get_device_list();
    while (dev) {
        bool is_hid = false;

        /* 设备级别的类码为 0x03 = HID 设备 */
        if (dev->dev_desc.bDeviceClass == 0x03) {
            is_hid = true;

        /* 设备级别类码为 0x00，需要检查接口描述符 */
        } else if (dev->dev_desc.bDeviceClass == 0x00) {
            uint8_t buffer[256];
            int ret = usb_control_transfer(dev->hc, dev->address,
                                           0x80, USB_REQ_GET_DESCRIPTOR,
                                           (USB_DESC_CONFIGURATION << 8) | 0,
                                           0, buffer, sizeof(buffer));
            if (ret < 0) {
                vga_write("[USB HID] Failed to get config desc.\n");
                dev = dev->next;
                continue;
            }
            usb_config_descriptor_t *cfg = (usb_config_descriptor_t*)buffer;
            uint16_t total = cfg->wTotalLength;
            if (total > sizeof(buffer)) total = sizeof(buffer);
            usb_interface_descriptor_t *iface = NULL;
            usb_endpoint_descriptor_t *ep_in = NULL, *ep_out = NULL;
            if (find_hid_interface(dev->hc, dev->address, buffer, total,
                                   &iface, &ep_in, &ep_out)) {
                is_hid = true;
                vga_write("[USB HID] Found HID interface.\n");
                ret = usb_control_transfer(dev->hc, dev->address,
                                           0x00, USB_REQ_SET_CONFIGURATION,
                                           cfg->bConfigurationValue, 0, NULL, 0);
                if (ret == 0) {
                    vga_write("[USB HID] Configuration set.\n");
                } else {
                    vga_write("[USB HID] Set config failed.\n");
                }
            }
        }

        if (is_hid) {
            vga_write("[USB HID] HID device found (VID=");
            vga_write_hex(dev->dev_desc.idVendor);
            vga_write(" PID=");
            vga_write_hex(dev->dev_desc.idProduct);
            vga_write(").\n");
        }

        dev = dev->next;
    }
    vga_write("[USB HID] Initialization complete (stub for interrupt transfers).\n");
}
