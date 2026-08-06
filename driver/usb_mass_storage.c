/*
 * usb_mass_storage.c - USB MSC（bInterfaceClass=0x08）驱动
 * 完整 Bulk-Only Transport：CBW(31B,OUT) → 数据(bulk) → CSW(13B,IN)，
 * 上层 SCSI 命令（INQUIRY/READ CAPACITY/READ10/WRITE10），扇区级读写。
 */

#include "../include/driver/usb.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/lib/string.h"
#include "../include/fs/vfs.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* MSC 子类和协议定义 */
#define MSC_SUBCLASS_SCSI      0x06   /* SCSI 透明命令集 */
#define MSC_PROTOCOL_BULK_ONLY 0x50   /* Bulk-Only Transport */

/* ---- BOT 协议结构 ---- */

/* CBW：Command Block Wrapper，31 字节，主机→设备 */
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;          /* 0x43425355 "USBC" */
    uint32_t dCBWTag;                /* 命令标签（CSW 回显） */
    uint32_t dCBWDataTransferLength; /* 数据阶段字节数 */
    uint8_t  bmCBWFlags;             /* bit7: 0=OUT, 1=IN */
    uint8_t  bCBWLUN;                /* 逻辑单元号（通常 0） */
    uint8_t  bCBWCBLength;           /* CB 长度（SCSI CDB 长度） */
    uint8_t  CB[16];                 /* SCSI 命令描述块 */
} msc_cbw_t;

/* CSW：Command Status Wrapper，13 字节，设备→主机 */
typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;          /* 0x53425355 "USBS" */
    uint32_t dCSWTag;                /* 与 CBW 一致 */
    uint32_t dCSWDataResidue;        /* 未传输完的字节数 */
    uint8_t  bCSWStatus;             /* 0=成功 */
} msc_csw_t;

/* ---- SCSI 命令操作码 ---- */
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

/* ---- 设备状态 ---- */

static bool msc_present = false;
static usb_hc_t *msc_hc = NULL;
static uint8_t msc_addr = 0;
static uint8_t msc_ep_in = 0;    /* 批量 IN 端点号 */
static uint8_t msc_ep_out = 0;   /* 批量 OUT 端点号 */
static uint32_t msc_sector_count = 0;
static uint32_t msc_sector_size = 512;
static uint32_t msc_cbw_tag = 1;

/* 找 MSC 接口（bInterfaceClass=0x08），记录批量端点（bmAttributes 低 2 位=0x02） */
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

/* BOT 协议失败恢复（规范 §5.3）：Mass Storage Reset 类请求 + 清两个
 * 批量端点 HALT，之后设备回到可收新 CBW 的状态 */
static void msc_bot_reset(void) {
    usb_control_transfer(msc_hc, msc_addr, 0x21, 0xFF, 0, 0, NULL, 0);  /* 0x21=主机→接口类, 0xFF=Reset */
    usb_control_transfer(msc_hc, msc_addr, 0x02, 0x01, 0, msc_ep_out, NULL, 0);  /* 清 OUT 端点 HALT */
    usb_control_transfer(msc_hc, msc_addr, 0x82, 0x01, 0, msc_ep_in, NULL, 0);   /* 清 IN 端点 HALT */
    volatile uint32_t s = 0;
    for (uint32_t j = 0; j < 2000000; j++) s++;
    (void)s;
}

/* 执行一次 BOT 命令，校验 CSW 签名/tag/状态；失败自动恢复并重试 */
static int msc_bot_command(const uint8_t *cb, uint8_t cb_len,
                           uint8_t dir, void *data, uint32_t data_len) {
    if (!msc_present) return -1;

    for (int attempt = 0; attempt < 3; attempt++) {
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
        if (ret == 0) {
            /* ⚠️ usb_bulk_bot 只等 CBW TD（ACTIVE 清位），链尾 DATA/CSW TD
             * 由控制器随后处理。快 vCPU 下不等 CSW 就返回，下一命令的 CBW
             * 会撞上设备还在 DATAOUT/CSW 态——QEMU usb-msd 直接 STALL
             * （真实设备是 NAK+重试，无此问题）。CSW 缓冲由设备异步填写
             * （与 TD 状态回写无关），轮询签名+tag 即可等到设备回 CBW 态。 */
            for (int i = 0; i < 200; i++) {
                if (csw.dCSWSignature == 0x53425355 && csw.dCSWTag == sent_tag) break;
                volatile uint32_t s = 0;
                for (uint32_t j = 0; j < 1000000; j++) s++;
                (void)s;
            }
            if (csw.dCSWSignature == 0x53425355 && csw.dCSWTag == sent_tag) {
                if (csw.bCSWStatus != 0) {
                    vga_write("[USB MSC] Command failed (status=");
                    vga_write_hex(csw.bCSWStatus);
                    vga_write(").\n");
                    return -1;
                }
                return 0;
            }
            vga_write("[USB MSC] CSW timeout, resetting...\n");
        } else {
            vga_write("[USB MSC] BOT transfer failed, resetting...\n");
        }
        msc_bot_reset();
    }
    return -1;
}

/* TEST UNIT READY：查设备是否就绪（无数据阶段） */
static int msc_test_unit_ready(void) {
    uint8_t cb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return msc_bot_command(cb, 6, 0, NULL, 0);
}

/* INQUIRY：拿厂商/产品字符串（36 字节应答） */
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

/* READ CAPACITY(10)：拿总扇区数与扇区大小 */
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

/* ---- 公开 API ---- */

/* 读一个扇区 */
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

/* 写一个扇区 */
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

/* 扇区数（0=无盘） */
uint32_t usb_msc_get_sector_count(void) {
    return msc_present ? msc_sector_count : 0;
}

/* 是否就绪 */
bool usb_msc_present(void) {
    return msc_present;
}

/* usb0 块设备后端（MSC bulk 传输），定义在文件末尾；sector_count 在探测后更新 */
static block_dev_t usb_block_dev;

/* 探测 MSC 设备：设配置 → TEST UNIT READY 重试 → INQUIRY/容量 */
void usb_mass_storage_init(void) {
    vga_write("[USB MSC] Scanning for Mass Storage devices...\n");

    usb_device_t *dev = usb_get_device_list();
    while (dev) {
        bool is_msc = false;

        if (dev->dev_desc.bDeviceClass == 0x08) {
            is_msc = true;
        } else {
            /* ⚠️ 修复：设备级 class 不可靠——QEMU 的 usb-storage 报 0x09，
             * 真机 U 盘也常有设备级 class 不规范的情况。
             * 正确做法：读配置描述符，按接口级 class（bInterfaceClass）
             * 判断是否 Mass Storage。旧实现只认 0x08/0x00，漏掉这类设备。 */
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

    /* usb0 注册到 VFS 并自动挂载到 /usb（根挂载 / 由 kmain 在 STEP 8 做） */
    if (msc_present) {
        usb_block_dev.sector_count = msc_sector_count;
        vfs_register_device(&usb_block_dev);
        vga_write("[USB MSC] usb0 registered, mounting /usb...\n");
        if (!vfs_mount("/usb", "usb0")) vga_write("[USB MSC] mount /usb failed\n");
    }

    vga_write("[USB MSC] Initialization complete.\n");
}

/* usb0 块设备后端（MSC bulk 传输） */
static bool usb_blk_read(uint32_t lba, void* buf) { return usb_msc_read_sector(lba, buf) == 0; }
static bool usb_blk_write(uint32_t lba, const void* buf) { return usb_msc_write_sector(lba, buf) == 0; }
static block_dev_t usb_block_dev = { "usb0", usb_blk_read, usb_blk_write, 0 };
