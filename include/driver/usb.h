/**
 * =========================================================================
 * usb.h - USB 子系统数据结构与接口定义
 *
 * USB（Universal Serial Bus）是一个主机-设备架构的总线系统。
 * HaoOS 实现了 UHCI（Universal Host Controller Interface）驱动，
 * 支持低速（1.5 Mbps）和全速（12 Mbps）USB 设备。
 *
 * 架构层级：
 *   USB 子系统 → UHCI 主控制器 → USB 总线 → USB 设备
 *                                       → HID 设备（键盘/鼠标）
 *                                       → Mass Storage（U 盘）
 *
 * URB（USB Request Block）：
 *   数据传输的基本单元，类似于网络协议中的数据包。支持四种传输类型：
 *   - 控制传输（Control）：设备枚举和配置，可靠双向
 *   - 中断传输（Interrupt）：鼠标、键盘等定期轮询
 *   - 批量传输（Bulk）：大块数据传输（U 盘读写）
 *   - 等时传输（Isochronous）：音视频实时数据
 *
 * 设备枚举流程：
 *   1. 复位端口 → 设备进入默认状态（地址 0）
 *   2. 获取设备描述符（前 8 字节确定 max packet size）
 *   3. 设置地址（分配唯一地址 1~127）
 *   4. 获取完整设备描述符
 *   5. 获取配置描述符（含接口和端点信息）
 *   6. 设置配置（设备进入配置状态）
 * =========================================================================
 */

#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stdbool.h>

/* ==================== 控制器类型 ==================== */
typedef enum {
    USB_HC_UHCI = 0x00,   /* Universal Host Controller Interface（Intel） */
    USB_HC_OHCI = 0x10,   /* Open Host Controller Interface（非 Intel） */
    USB_HC_EHCI = 0x20,   /* Enhanced HC（USB 2.0 高速） */
    USB_HC_XHCI = 0x30,   /* eXtensible HC（USB 3.x） */
    USB_HC_UNKNOWN
} usb_hc_type_t;

/* 前向声明 */
typedef struct usb_hc usb_hc_t;

/* ==================== URB（USB 请求块） ==================== */

/**
 * usb_urb_t - USB 请求块
 * 描述一次 USB 数据传输请求，包括端点、方向、缓冲区和完成回调。
 */
typedef struct usb_urb {
    uint8_t  type;          /* 传输类型：0=控制, 1=中断, 2=批量, 3=等时 */
    uint8_t  dev_addr;      /* 目标设备地址（1~127） */
    uint8_t  endpoint;      /* 端点号（不含方向位，方向由 direction 字段指定） */
    uint8_t  direction;     /* 传输方向：0=OUT（主机→设备）, 1=IN（设备→主机） */
    uint16_t length;        /* 数据长度（字节） */
    void    *buffer;        /* 数据缓冲区指针 */
    void    (*callback)(struct usb_urb *urb);  /* 完成回调函数 */
    uint8_t  req_type;      /* 控制传输专用：请求类型（bmRequestType） */
    uint8_t  request;       /* 控制传输专用：请求号（bRequest） */
    uint16_t value;         /* 控制传输专用：wValue */
    uint16_t index;         /* 控制传输专用：wIndex */
} usb_urb_t;

/* URB 类型常量 */
#define USB_URB_CONTROL    0    /* 控制传输 */
#define USB_URB_INTERRUPT  1    /* 中断传输 */
#define USB_URB_BULK       2    /* 批量传输 */
#define USB_URB_ISO        3    /* 等时传输 */

/* ==================== USB 标准请求 ==================== */
#define USB_REQ_GET_STATUS        0x00  /* 获取设备/端点状态 */
#define USB_REQ_CLEAR_FEATURE     0x01  /* 清除特征 */
#define USB_REQ_SET_FEATURE       0x03  /* 设置特征 */
#define USB_REQ_SET_ADDRESS       0x05  /* 设置设备地址 */
#define USB_REQ_GET_DESCRIPTOR    0x06  /* 获取描述符 */
#define USB_REQ_SET_DESCRIPTOR    0x07  /* 设置描述符 */
#define USB_REQ_GET_CONFIGURATION 0x08  /* 获取当前配置值 */
#define USB_REQ_SET_CONFIGURATION 0x09  /* 设置配置 */
#define USB_REQ_GET_INTERFACE     0x0A  /* 获取当前备用接口 */
#define USB_REQ_SET_INTERFACE     0x0B  /* 设置备用接口 */
#define USB_REQ_SYNCH_FRAME       0x0C  /* 设置/读取同步帧 */

/* ==================== USB 描述符类型 ==================== */
#define USB_DESC_DEVICE           0x01  /* 设备描述符 */
#define USB_DESC_CONFIGURATION    0x02  /* 配置描述符 */
#define USB_DESC_STRING           0x03  /* 字符串描述符 */
#define USB_DESC_INTERFACE        0x04  /* 接口描述符 */
#define USB_DESC_ENDPOINT         0x05  /* 端点描述符 */
#define USB_DESC_DEVICE_QUALIFIER 0x06  /* 设备限定符 */
#define USB_DESC_OTHER_SPEED      0x07  /* 其他速度配置 */
#define USB_DESC_INTERFACE_POWER  0x08  /* 接口电源 */
#define USB_DESC_HID              0x21  /* HID 描述符 */
#define USB_DESC_REPORT           0x22  /* HID 报告描述符 */

/* ==================== USB 标准描述符结构 ==================== */

/**
 * usb_device_descriptor_t - USB 设备描述符（18 字节）
 * 描述 USB 设备的通用信息：USB 版本、设备类、厂商/产品 ID 等。
 */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;            /* 描述符长度（= 18） */
    uint8_t  bDescriptorType;    /* 描述符类型（= 0x01） */
    uint16_t bcdUSB;             /* USB 规范版本号（BCD 编码，如 0x0110 = USB 1.1） */
    uint8_t  bDeviceClass;       /* 设备类码（0x00=接口定义, 0x03=HID, 0x08=MSC） */
    uint8_t  bDeviceSubClass;    /* 设备子类码 */
    uint8_t  bDeviceProtocol;    /* 设备协议码 */
    uint8_t  bMaxPacketSize0;    /* 端点 0 最大包大小（8/16/32/64） */
    uint16_t idVendor;           /* 厂商 ID（如 Intel=0x8086） */
    uint16_t idProduct;          /* 产品 ID */
    uint16_t bcdDevice;          /* 设备版本号（BCD 编码） */
    uint8_t  iManufacturer;      /* 厂商字符串描述符索引 */
    uint8_t  iProduct;           /* 产品字符串描述符索引 */
    uint8_t  iSerialNumber;      /* 序列号字符串描述符索引 */
    uint8_t  bNumConfigurations; /* 支持的配置数量 */
} usb_device_descriptor_t;

/**
 * usb_config_descriptor_t - USB 配置描述符
 * 描述一个配置的总体信息：接口数量、供电方式、最大功耗等。
 * 后面跟着 bNumInterfaces 个接口描述符。
 */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;            /* = 9 */
    uint8_t  bDescriptorType;    /* = 0x02 */
    uint16_t wTotalLength;       /* 配置描述符树总长度（含接口和端点） */
    uint8_t  bNumInterfaces;     /* 此配置包含的接口数 */
    uint8_t  bConfigurationValue; /* 此配置的编号（用于 SetConfiguration） */
    uint8_t  iConfiguration;     /* 配置字符串描述符索引 */
    uint8_t  bmAttributes;       /* 属性（bit7=总线供电, bit6=自供电, bit5=远程唤醒） */
    uint8_t  bMaxPower;          /* 最大功耗（单位 2mA） */
} usb_config_descriptor_t;

/**
 * usb_interface_descriptor_t - USB 接口描述符
 * 描述 USB 设备的一个功能接口。一个配置可以有多个接口，
 * 一个接口可以有多个备用设置。
 */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;            /* = 9 */
    uint8_t  bDescriptorType;    /* = 0x04 */
    uint8_t  bInterfaceNumber;   /* 接口号（从 0 开始） */
    uint8_t  bAlternateSetting;  /* 备用设置编号 */
    uint8_t  bNumEndpoints;      /* 此接口的端点数量（不含端点 0） */
    uint8_t  bInterfaceClass;    /* 接口类码（0x03=HID, 0x08=MSC） */
    uint8_t  bInterfaceSubClass; /* 接口子类码 */
    uint8_t  bInterfaceProtocol; /* 接口协议码 */
    uint8_t  iInterface;         /* 接口字符串描述符索引 */
} usb_interface_descriptor_t;

/**
 * usb_endpoint_descriptor_t - USB 端点描述符
 * 描述一个端点的地址、传输类型和最大包大小。
 *
 * bEndpointAddress 格式：
 *   bit 0-3: 端点号
 *   bit 7:   方向（0=OUT, 1=IN）
 *
 * bmAttributes 格式：
 *   bit 0-1: 传输类型（00=控制, 01=等时, 10=批量, 11=中断）
 */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;            /* = 7 */
    uint8_t  bDescriptorType;    /* = 0x05 */
    uint8_t  bEndpointAddress;   /* 端点地址和方向 */
    uint8_t  bmAttributes;       /* 端点属性 */
    uint16_t wMaxPacketSize;     /* 最大包大小 */
    uint8_t  bInterval;          /* 轮询间隔（帧数，1=每帧一次） */
} usb_endpoint_descriptor_t;

/**
 * usb_hid_descriptor_t - HID 描述符
 * 描述 HID 设备的协议版本和补充描述符列表。
 */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;            /* = 9 */
    uint8_t  bDescriptorType;    /* = 0x21 */
    uint16_t bcdHID;             /* HID 规范版本号 */
    uint8_t  bCountryCode;       /* 国家代码 */
    uint8_t  bNumDescriptors;    /* 补充描述符数量（通常为 1 个报告描述符） */
} usb_hid_descriptor_t;

/* ==================== USB 设备结构体 ==================== */

/**
 * usb_device_t - USB 设备抽象
 * 表示一个已枚举的 USB 设备。
 */
typedef struct usb_device {
    uint8_t               address;       /* 设备地址（1~127） */
    usb_hc_t             *hc;            /* 所属的主控制器 */
    usb_device_descriptor_t dev_desc;    /* 设备描述符 */
    uint8_t               num_configs;   /* 配置数量 */
    struct usb_device    *next;          /* 链表下一个设备 */
} usb_device_t;

/**
 * usb_hc_t - USB 主控制器抽象
 * 每个检测到的 UHCI/OHCI/EHCI 控制器对应一个实例。
 */
struct usb_hc {
    usb_hc_type_t type;         /* 控制器类型 */
    uint32_t      base_addr;    /* I/O 基址或 MMIO 基址 */
    uint8_t       bus;          /* PCI 总线号 */
    uint8_t       slot;         /* PCI 设备号 */
    uint8_t       func;         /* PCI 功能号 */
    void          (*init)(usb_hc_t *hc);        /* 初始化函数 */
    int           (*submit_urb)(usb_hc_t *hc, usb_urb_t *urb); /* URB 提交 */
};

/* ==================== 基础函数声明 ==================== */

void usb_init(void);                 /* USB 子系统初始化 */

void usb_poll(void);                 /* 轮询处理中断传输完成 */

/* 设备列表管理 */
void usb_register_device(usb_device_t *dev);
usb_device_t* usb_get_device_list(void);

/* 控制器注册 */
void usb_register_hc(usb_hc_t *hc);

/* URB 提交接口 */
int usb_submit_urb(usb_hc_t *hc, usb_urb_t *urb);

/* 高层 API：控制传输 */
int usb_control_transfer(usb_hc_t *hc, uint8_t dev_addr,
                         uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex,
                         void *data, uint16_t wLength);


/* 高层 API：批量传输（BOT：CBW→DATA→CSW 三次事务，同步执行） */
int usb_bulk_bot(usb_hc_t *hc, uint8_t dev_addr,
                 uint8_t ep_out, uint8_t ep_in,
                 const void *cbw, uint16_t cbw_len,
                 void *data, uint32_t data_len, uint8_t dir,
                 void *csw, uint16_t csw_len);
/* ==================== 设备枚举 ==================== */

int usb_set_address(usb_hc_t *hc, uint8_t dev_addr);
int usb_get_device_descriptor(usb_hc_t *hc, uint8_t dev_addr,
                               usb_device_descriptor_t *desc);
int usb_get_config_descriptor(usb_hc_t *hc, uint8_t dev_addr,
                               uint8_t config_idx,
                               uint8_t *buffer, uint16_t buf_size);
int usb_set_configuration(usb_hc_t *hc, uint8_t dev_addr, uint8_t config_value);
void usb_enumerate_device(usb_hc_t *hc);    /* 枚举单个设备 */
void usb_enumerate_all(void);               /* 枚举全部设备 */

/* ==================== 辅助函数 ==================== */

/**
 * usb_parse_configuration - 解析配置描述符，查找指定类接口
 *
 * 遍历配置描述符树的接口和端点描述符，找到匹配
 * interface_class/interface_subclass 的接口，返回其接口描述符
 * 和批量输入/输出端点。
 */
bool usb_parse_configuration(const uint8_t *config_data, uint16_t total_len,
                             usb_interface_descriptor_t **out_iface,
                             usb_endpoint_descriptor_t **out_ep_in,
                             usb_endpoint_descriptor_t **out_ep_out,
                             uint8_t interface_class, uint8_t interface_subclass);

/* ==================== 子模块初始化 ==================== */

void usb_hid_init(void);               /* HID 子模块初始化 */
void usb_mass_storage_init(void);      /* Mass Storage 初始化 */
void usb_mouse_init(void);             /* USB 鼠标初始化 */

/* Mass Storage 高层 API（扇区级读写） */
int usb_msc_read_sector(uint32_t lba, void *buf);
int usb_msc_write_sector(uint32_t lba, const void *buf);
uint32_t usb_msc_get_sector_count(void);
bool usb_msc_present(void);

/* HID 接口查找函数（全局，供 mouse.c 等调用） */
bool find_hid_interface(usb_hc_t *hc, uint8_t dev_addr,
                        const uint8_t *config_data, uint16_t total_len,
                        usb_interface_descriptor_t **out_iface,
                        usb_endpoint_descriptor_t **out_ep_in,
                        usb_endpoint_descriptor_t **out_ep_out);

#endif
