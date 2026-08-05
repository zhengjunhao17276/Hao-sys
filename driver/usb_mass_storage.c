/**
 * =========================================================================
 * usb_mass_storage.c - USB Mass Storage（大容量存储）驱动
 *
 * USB MSC（Mass Storage Class, bInterfaceClass = 0x08）涵盖 U 盘、
 * 移动硬盘等存储设备。本驱动实现了完整的 Bulk-Only Transport（BOT）
 * 协议和常用 SCSI 命令，支持扇区级读写。
 *
 * Mass Storage 协议栈：
 *   应用层 → SCSI 命令（READ/WRITE/INQUIRY...）
 *           → BOT (CBW/CSW 包裹)
 *           → Bulk 传输（USB 批量端点）
 *           → USB 总线
 *
 * BOT 命令流程（每次 SCSI 命令）：
 *   1. 主机 → 设备：CBW（31 字节，bulk OUT，DATA0）
 *      - dCBWSignature = 0x43425355（"USBC"）
 *      - dCBWTag：命令标签，CSW 必须回显
 *      - dCBWDataTransferLength：数据阶段长度
 *      - bmCBWFlags：0x80 = 数据阶段设备→主机（IN），0x00 = 主机→设备
 *      - bCBWCBLength：SCSI CDB 长度
 *      - CB[16]：SCSI 命令描述块
 *   2. 数据阶段（可选）：bulk IN/OUT，长度由 CBW 指定
 *   3. 设备 → 主机：CSW（13 字节，bulk IN）
 *      - dCSWSignature = 0x53425355（"USBS"）
 *      - dCSWTag：必须与 CBW 的 tag 一致
 *      - bCSWStatus：0 = 命令成功
 * =========================================================================
 */

#include "../include/driver/usb.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/lib/string.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* MSC 子类和协议定义 */
#define MSC_SUBCLASS_SCSI      0x06   /* SCSI 透明命令集 */
#define MSC_PROTOCOL_BULK_ONLY 0x50   /* Bulk-Only Transport */

/* ==================== BOT 协议结构 ==================== */

/** CBW - Command Block Wrapper（31 字节，主机→设备） */
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;          /* 0x43425355 "USBC" */
    uint32_t dCBWTag;                /* 命令标签（CSW 回显） */
    uint32_t dCBWDataTransferLength; /* 数据阶段字节数 */
    uint8_t  bmCBWFlags;             /* bit7: 0=OUT, 1=IN */
    uint8_t  bCBWLUN;                /* 逻辑单元号（通常 0） */
    uint8_t  bCBWCBLength;           /* CB 长度（SCSI CDB 长度） */
    uint8_t  CB[16];                 /* SCSI 命令描述块 */
} msc_cbw_t;

/** CSW - Command Status Wrapper（13 字节，设备→主机） */
typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;          /* 0x53425355 "USBS" */
    uint32_t dCSWTag;                /* 与 CBW 一致 */
    uint32_t dCSWDataResidue;        /* 未传输完的字节数 */
    uint8_t  bCSWStatus;             /* 0=成功 */
} msc_csw_t;

/* ==================== SCSI 命令操作码 ==================== */
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

/* ==================== 设备状态 ==================== */

static bool msc_present = false;
static usb_hc_t *msc_hc = NULL;
static uint8_t msc_addr = 0;
static uint8_t msc_ep_in = 0;    /* 批量 IN 端点号 */
static uint8_t msc_ep_out = 0;   /* 批量 OUT 端点号 */
static uint32_t msc_sector_count = 0;
static uint32_t msc_sector_size = 512;
static uint32_t msc_cbw_tag = 1;

/**
 * find_msc_interface - 在配置描述符中查找 Mass Storage 接口
 * 解析配置描述符树，寻找 bInterfaceClass == 0x08 的接口，
 * 记录其批量输入/输出端点（bmAttributes 低 2 位 = 0x02 批量）。
 */
static bool find_msc_interface(usb_hc_t *hc, uint8_t dev_addr,
                               const uint8_t *config_data, uint16_t total_len,
                               usb_interface_descriptor_t **out_iface,
                               usb_endpoint_descriptor_t **out_ep_in,
                               usb_endpoint_descriptor_t **out_ep_out) {
    (void)hc;
    (void)dev_addr;
    uint16_t pos = 0;
    while (pos < total_len) {
        uint8_t len = config_data[pos];
        if (len == 0) break;
        uint8_t type = config_data[pos+1];

        if (type == USB_DESC_INTERFACE) {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t*)&config_data[pos];
            if (iface->bInterfaceClass == 0x08) {  /* Mass Storage Class */
                *out_iface = iface;
                pos += len;
                while (pos < total_len) {
                    uint8_t l = config_data[pos];
                    if (l == 0) break;
                    uint8_t t = config_data[pos+1];
                    if (t == USB_DESC_ENDPOINT) {
                        usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t*)&config_data[pos];
                        if ((ep->bmAttributes & 0x03) != 0x02) { pos += l; continue; }  /* 只要批量端点 */
                        if ((ep->bEndpointAddress & 0x80) && !*out_ep_in)
                            *out_ep_in = ep;
                        else if (!(ep->bEndpointAddress & 0x80) && !*out_ep_out)
                            *out_ep_out = ep;
                    } else if (t == USB_DESC_INTERFACE) {
                        break;
                    }
                    pos += l;
                }
                return true;
            }
        }
        pos += len;
    }
    return false;
}

/**
 * msc_bot_command - 执行一次 BOT 命令（CBW → 数据 → CSW）
 * @cb:       SCSI CDB
 * @cb_len:   CDB 长度
 * @dir:      数据方向：1=IN（设备→主机），0=OUT
 * @data:     数据缓冲区
 * @data_len: 数据长度
 * 返回 0=成功，-1=失败
 */
static int msc_bot_command(const uint8_t *cb, uint8_t cb_len,
                           uint8_t dir, void *data, uint32_t data_len) {
    if (!msc_present) return -1;

    msc_cbw_t cbw;
    msc_csw_t csw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = 0x43425355;
    uint32_t sent_tag = msc_cbw_tag++;
    cbw.dCBWTag = sent_tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = dir ? 0x80 : 0x00;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cb_len;
    memcpy(cbw.CB, cb, cb_len);

    int ret = usb_bulk_bot(msc_hc, msc_addr, msc_ep_out, msc_ep_in,
                           &cbw, sizeof(cbw),
                           data, data_len, dir,
                           &csw, sizeof(csw));
    if (ret != 0) {
        vga_write("[USB MSC] BOT transfer failed.\n");
        return -1;
    }
    if (csw.dCSWSignature != 0x53425355) {
        vga_write("[USB MSC] Bad CSW signature.\n");
        return -1;
    }
    if (csw.dCSWTag != sent_tag) {
        vga_write("[USB MSC] CSW tag mismatch (");
        vga_write_hex(csw.dCSWTag);
        vga_write(" vs ");
        vga_write_hex(sent_tag);
        vga_write(").\n");
        return -1;
    }
    if (csw.bCSWStatus != 0) {
        vga_write("[USB MSC] Command failed (status=");
        vga_write_hex(csw.bCSWStatus);
        vga_write(").\n");
        return -1;
    }
    return 0;
}

/**
 * msc_test_unit_ready - SCSI TEST UNIT READY
 * 询问设备是否就绪（无数据阶段）。
 */
static int msc_test_unit_ready(void) {
    uint8_t cb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return msc_bot_command(cb, 6, 0, NULL, 0);
}

/**
 * msc_inquiry - SCSI INQUIRY
 * 获取设备厂商/产品信息（36 字节标准应答）。
 */
static int msc_inquiry(void) {
    uint8_t cb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t data[36];
    memset(data, 0, sizeof(data));
    int ret = msc_bot_command(cb, 6, 1, data, sizeof(data));
    if (ret == 0) {
        vga_write("[USB MSC] INQUIRY: ");
        for (int i = 8; i < 36; i++) {
            char c = (char)data[i];
            if (c >= 0x20 && c < 0x7F) vga_putchar(c);
            else vga_putchar(' ');
        }
        vga_write("\n");
    }
    return ret;
}

/**
 * msc_read_capacity - SCSI READ CAPACITY(10)
 * 获取总扇区数和扇区大小（8 字节应答）。
 */
static int msc_read_capacity(void) {
    uint8_t cb[10] = { SCSI_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t data[8];
    memset(data, 0, sizeof(data));
    int ret = msc_bot_command(cb, 10, 1, data, sizeof(data));
    if (ret == 0) {
        /* 应答 = 最后一个 LBA（4 字节大端）+ 扇区大小（4 字节大端） */
        uint32_t last_lba = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                          | ((uint32_t)data[2] << 8) | data[3];
        msc_sector_count = last_lba + 1;
        msc_sector_size = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16)
                        | ((uint32_t)data[6] << 8) | data[7];
        vga_write("[USB MSC] Capacity: ");
        vga_write_hex(msc_sector_count);
        vga_write(" sectors x ");
        vga_write_hex(msc_sector_size);
        vga_write("B\n");
    }
    return ret;
}

/* ==================== 公开 API ==================== */

/**
 * usb_msc_read_sector - 从 USB 盘读取一个扇区
 */
int usb_msc_read_sector(uint32_t lba, void *buf) {
    if (!msc_present) return -1;
    uint8_t cb[10] = {
        SCSI_READ10, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)(lba),
        0, 0, 1, 0            /* 传输长度 = 1 个扇区 */
    };
    return msc_bot_command(cb, 10, 1, buf, msc_sector_size);
}

/**
 * usb_msc_write_sector - 向 USB 盘写入一个扇区
 */
int usb_msc_write_sector(uint32_t lba, const void *buf) {
    if (!msc_present) return -1;
    uint8_t cb[10] = {
        SCSI_WRITE10, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)(lba),
        0, 0, 1, 0
    };
    return msc_bot_command(cb, 10, 0, (void*)buf, msc_sector_size);
}

/** 获取 USB 盘扇区数（0 = 无盘） */
uint32_t usb_msc_get_sector_count(void) {
    return msc_present ? msc_sector_count : 0;
}

/** USB 盘是否就绪 */
bool usb_msc_present(void) {
    return msc_present;
}

/**
 * usb_mass_storage_init - 探测并初始化 USB Mass Storage 设备
 * 遍历 USB 设备列表，识别 MSC 接口，设置配置，
 * 执行 INQUIRY / READ CAPACITY 确定容量。
 */
void usb_mass_storage_init(void) {
    vga_write("[USB MSC] Scanning for Mass Storage devices...\n");

    usb_device_t *dev = usb_get_device_list();
    while (dev) {
        bool is_msc = false;

        if (dev->dev_desc.bDeviceClass == 0x08) {
            is_msc = true;
        } else if (dev->dev_desc.bDeviceClass == 0x00) {
            uint8_t buffer[256];
            int ret = usb_control_transfer(dev->hc, dev->address,
                                           0x80, USB_REQ_GET_DESCRIPTOR,
                                           (USB_DESC_CONFIGURATION << 8) | 0,
                                           0, buffer, sizeof(buffer));
            if (ret < 0) {
                dev = dev->next;
                continue;
            }

            usb_config_descriptor_t *cfg = (usb_config_descriptor_t*)buffer;
            uint16_t total = cfg->wTotalLength;
            if (total > sizeof(buffer)) total = sizeof(buffer);

            usb_interface_descriptor_t *iface = NULL;
            usb_endpoint_descriptor_t *ep_in = NULL, *ep_out = NULL;
            if (find_msc_interface(dev->hc, dev->address, buffer, total,
                                   &iface, &ep_in, &ep_out)) {
                is_msc = true;
                vga_write("[USB MSC] Found Mass Storage interface.\n");

                ret = usb_control_transfer(dev->hc, dev->address,
                                           0x00, USB_REQ_SET_CONFIGURATION,
                                           cfg->bConfigurationValue, 0, NULL, 0);
                if (ret == 0 && ep_in && ep_out) {
                    vga_write("[USB MSC] Configuration set.\n");
                    msc_present = true;
                    msc_hc = dev->hc;
                    msc_addr = dev->address;
                    msc_ep_in = ep_in->bEndpointAddress & 0x0F;
                    msc_ep_out = ep_out->bEndpointAddress & 0x0F;
                    msc_sector_count = 0;

                    vga_write("[USB MSC] bulk ep in=");
                    vga_write_hex(msc_ep_in);
                    vga_write(" out=");
                    vga_write_hex(msc_ep_out);
                    vga_write("\n");

                    /* 设备可能尚未就绪（旋转/复位），重试 TEST UNIT READY */
                    int ready = -1;
                    for (int i = 0; i < 10 && ready != 0; i++) {
                        ready = msc_test_unit_ready();
                        if (ready != 0) {
                            volatile uint32_t s = 0;
                            for (uint32_t j = 0; j < 20000000; j++) s++;
                            (void)s;
                        }
                    }
                    if (ready != 0) {
                        vga_write("[USB MSC] Device not ready.\n");
                        msc_present = false;
                    } else {
                        msc_inquiry();
                        msc_read_capacity();
                    }
                } else {
                    vga_write("[USB MSC] Set config failed or no bulk eps.\n");
                }
            }
        }

        if (is_msc) {
            vga_write("[USB MSC] Device found (VID=");
            vga_write_hex(dev->dev_desc.idVendor);
            vga_write(" PID=");
            vga_write_hex(dev->dev_desc.idProduct);
            vga_write(").\n");
        }
        dev = dev->next;
    }
    vga_write("[USB MSC] Initialization complete.\n");
}
