#include "../include/driver/io.h"
#include "../include/driver/usb.h"
#include "../include/driver/pci.h"
#include "../include/driver/vga.h"
#include "../include/mm/pmm.h"
#include "../include/mm/vmm.h"
#include "../include/lib/string.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * usb_controller.c - UHCI 主机控制器驱动
 * 寄存器走 I/O 端口（相对 BAR0 偏移）；帧列表 1024 项 + 预分配 TD 池；
 * 控制/中断/批量（BOT）三种传输，全部轮询等待完成。
 */

/* 0x00 — 命令寄存器：全局复位、启停帧列表处理 */
#define UHCI_CMD       0x00

/* 0x02 — 状态寄存器：USBINT/USBERR 等状态位，写 1 清零 */
#define UHCI_STS       0x02

/* 0x04 — 中断使能：USBINT/USBERR 等事件是否触发中断 */
#define UHCI_INTR      0x04

/* 0x06 — 帧号寄存器（0~1023，每 1ms 一帧） */
#define UHCI_FRNUM     0x06

/* 0x08 — 帧列表基址（物理地址，1024 项指针，须在启动前设定） */
#define UHCI_FRBASE    0x08

/* 0x0C — SOF 定时微调寄存器 */
#define UHCI_SOF       0x0C

/* 0x10/0x12 — 端口 1/2 状态与控制（连接检测、使能、复位、速度） */
#define UHCI_PORT1     0x10
#define UHCI_PORT2     0x12


/*
 *  UHCI 命令寄存器（CMD）中各控制位的定义
 */

/* RS：写 1 启动帧列表处理（发 SOF），写 0 停止 */
#define UHCI_CMD_RS     (1 << 0)

/* HCRESET：控制器软件复位，寄存器恢复默认值 */
#define UHCI_CMD_HC     (1 << 1)

/* GRESET：总线全局复位（≥10ms），设备回到默认地址 0 */
#define UHCI_CMD_GRESET (1 << 2)

/* CF：配置完成标志，CF=1 前端口状态变化不产生中断 */
#define UHCI_CMD_CF     (1 << 6)


/*
 *  UHCI 状态寄存器（STS）中的状态位
 */

/* USBINT：传输完成（IOC 位 TD 执行成功），写 1 清零 */
#define UHCI_STS_USBINT (1 << 0)

/* USBERR：传输错误（STALL/CRC/Babble 等），写 1 清零 */
#define UHCI_STS_USBERR (1 << 1)


/*
 *  UHCI 端口状态与控制寄存器的位定义
 *  每一位都对应端口的一个硬件信号或状态。
 */

/* CCS：当前连接状态（1=有设备） */
#define UHCI_PORT_CCS   (1 << 0)

/* CSC：连接状态变化标志，写 1 清零确认 */
#define UHCI_PORT_CSC   (1 << 1)

/* PE：端口使能，硬件/错误条件可自动清零 */
#define UHCI_PORT_PE    (1 << 2)

/* PEC：端口使能变化标志，写 1 清零 */
#define UHCI_PORT_PEC   (1 << 3)

/* LSDA：1=低速设备（1.5Mbps），0=全速（12Mbps） */
#define UHCI_PORT_LSDA  (1 << 4)

/* RD：写 1 发起端口复位（≥10ms），硬件完成后自动清零 */
#define UHCI_PORT_RD    (1 << 5)

/* SUSP：端口挂起（省电） */
#define UHCI_PORT_SUSP  (1 << 7)


/* 帧列表 1024 项（1ms/帧，帧号 0-1023 循环）；TD 池 64 个，
 * TD 硬件要求 16 字节对齐 */
#define FRAME_LIST_SIZE 1024
#define MAX_TDS         64

/*
 * TD（Transfer Descriptor，16 字节，物理地址低 4 位须为 0）：
 * link（下个 TD/QH 指针，bit0=1 为 TD、bit0=0 为 QH，bit1=VF 垂直结束）
 * + ctrl_status（控制/状态字）+ token（PID/地址/端点/长度）+ buffer_ptr。
 * 硬件沿帧列表 → TD 链逐项发起总线事务。注意 TD 是 UHCI 调度单位，
 * 跟 USB 协议包（token/data/handshake）是两个层次的概念。
 */
typedef struct uhci_td {
    uint32_t link;           /* 下个 TD/QH 物理地址；bit0=1 为 TD，bit1=VF */
    uint32_t ctrl_status;    /* 高位状态位（硬件更新）+ 低位控制位（IOC/SPD） */
    uint32_t token;          /* 令牌：PID、设备地址、端点号、数据长度 */
    uint32_t buffer_ptr;     /* 数据缓冲区物理地址（DMA 直接访问） */
} __attribute__((aligned(16))) uhci_td_t;

/* QH（8 字节）。qemu 要求帧列表指向 QH（bit0=0, bit1=1），TD 链从
 * el_link 垂直挂下——直接把 TD 挂帧列表，控制传输组装不成
 * setup→data→status 一次事务，设备必然 STALL。 */
typedef struct __attribute__((aligned(16))) {
    uint32_t link;      /* 水平链接：下一个 QH，或 0x01 终止 */
    uint32_t el_link;   /* 垂直链接：第一个 TD（bit0=0），0x01=空 */
} uhci_qh_t;


/*
 *  TD 控制/状态字（ctrl_status 字段）中各位的定义
 *  注意：控制位由软件设置，状态位由硬件更新
 */

/* ACTIVE：软件置 1 待处理，硬件完成后清零（出错也会清） */
#define TD_CTRL_ACTIVE   (1 << 23)

/* STALL：设备返回 STALL（不支持该请求或端点 halted） */
#define TD_CTRL_STALL    (1 << 22)

/* DBUF：DMA 访问数据缓冲区出错（通常是软件 bug） */
#define TD_CTRL_DBUF     (1 << 21)

/* BABBLE：设备发送超过预期长度的数据 */
#define TD_CTRL_BABBLE   (1 << 20)

/* NAK：设备暂时无法处理（不是错误），UHCI 自动重试 */
#define TD_CTRL_NAK      (1 << 19)

/* CRC/Timeout：CRC 校验失败或设备无响应 */
#define TD_CTRL_CRCTIMEO (1 << 18)

/* BITSTUFF：NRZI 位填充违规 */
#define TD_CTRL_BITSTUFF (1 << 17)

/* LENERR：实际数据长度与令牌期望不匹配 */
#define TD_CTRL_LENERR   (1 << 16)

/* 软件设置的控制位 */

/* IOC：TD 完成时触发 USBINT 中断（状态阶段用） */
#define TD_CTRL_IOC      (1 << 7)

/* SPD：收到短包视为完成（批量传输短包检测用） */
#define TD_CTRL_SPD      (1 << 8)


/* 令牌 PID：SETUP=设置阶段（8 字节标准请求）、OUT=主机→设备、
 * IN=设备→主机。规范中令牌包是 4 位 PID + 4 位反码，宏给完整字节值。 */
#define TD_TOKEN_PID_SETUP  0x2D
#define TD_TOKEN_PID_OUT    0xE1
#define TD_TOKEN_PID_IN     0x69

/* 控制器私有数据（本驱动只支持单控制器，静态分配） */
typedef struct {
    uint16_t io_base;              /* I/O 端口基地址（来自 PCI BAR0） */
    uint32_t *frame_list;          /* 帧列表虚拟地址（1024 个 32 位指针） */
    uint32_t  frame_list_phys;     /* 帧列表物理地址（写入 FRBASE 寄存器） */
    uhci_td_t *td_pool;            /* TD 池虚拟地址 */
    uint32_t  td_pool_phys;        /* TD 池物理地址 */
    int       td_used;             /* TD 池使用计数 */
    bool      running;             /* 控制器运行状态 */
    uint8_t   next_address;        /* 下一个可分配的 USB 设备地址 */
} uhci_private_t;

/* 全局 UHCI 控制器私有数据实例。初始化为全零。 */
static uhci_private_t uhci_priv = {0};

/* 调度 QH：所有帧列表项都指向它，TD 链从它垂直挂下 */
static uhci_qh_t *schedule_qh = NULL;
static uint32_t schedule_qh_phys = 0;

/* DMA 内存辅助：帧列表/TD 池要物理连续，且硬件只见物理地址 */
static void* alloc_phys_pages(int count, uint32_t *phys) {
    /* ⚠️ 架构升级：用 pmm_alloc_contiguous 一次性分配连续页
     * （O(1) 空闲链表分配不保证连续；旧实现逐个分配 + 连续性检查，
     * 碎片化时失败率高）。 */
    void* first = pmm_alloc_contiguous((uint32_t)count);
    if (!first) return NULL;
    *phys = (uint32_t)(uintptr_t)first;
    return first;
}

/* 释放通过 alloc_phys_pages 分配的连续物理页（当前未使用，保留供后续错误恢复） */
static void __attribute__((unused)) free_phys_pages(void *virt, int count) {
    for (int i = 0; i < count; i++)
        pmm_free_page((void*)((uintptr_t)virt + i * 4096));
}

/* 寄存器访问封装：UHCI 走 I/O 端口（16 位寄存器用 word，FRBASE 用 long） */
static inline void uhci_writew(uint16_t reg, uint16_t val) {
    outw(uhci_priv.io_base + reg, val);
}
static inline uint16_t uhci_readw(uint16_t reg) {
    return inw(uhci_priv.io_base + reg);
}
static inline void uhci_writel(uint16_t reg, uint32_t val) {
    outl(uhci_priv.io_base + reg, val);
}
static inline uint32_t uhci_readl(uint16_t reg) {
    return inl(uhci_priv.io_base + reg);
}

/* TD 池：顺序取用、控制传输结束后回滚计数（简化实现，无空闲链表） */
/* 忙等延时：复位/枚举要 ≥10ms 稳定时间。之前用 nop 凑（~100ns）
 * 设备根本没复位完，首个控制传输必然 NAK 超时。 */
static void uhci_delay_ms(int ms) {
    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < (uint32_t)ms * 200000u; i++) sink++;
    (void)sink;
}

static uhci_td_t* uhci_alloc_td(void) {
    if (uhci_priv.td_used >= MAX_TDS) return NULL;
    return &uhci_priv.td_pool[uhci_priv.td_used++];
}

/* ⚠️ 修复（USB 留意项 4）：线性池的栈式释放——只释放最近分配的 TD。
 * 用于中断首提失败路径，避免 TD 泄漏（池 64 个，泄漏几次就耗尽）。 */
static void uhci_free_td(uhci_td_t* td) {
    if (td >= &uhci_priv.td_pool[0] && td < &uhci_priv.td_pool[uhci_priv.td_used]) {
        uhci_priv.td_used--;
    }
}

static void uhci_setup_td(uhci_td_t *td, uint32_t link, uint32_t ctrl_status,
                          uint32_t token, uint32_t buffer) {
    td->link = link;
    td->ctrl_status = ctrl_status | TD_CTRL_ACTIVE;  /* 置 ACTIVE 交给硬件 */
    td->token = token;
    td->buffer_ptr = buffer;
}

/* 轮询等 TD 完成：控制传输微秒级完成，不值得上中断 */
static bool uhci_wait_td(uhci_td_t *td, int timeout_us) {
    (void)timeout_us;
    /* ⚠️ 关键：帧列表里同一帧每 1024 帧才被处理一次（≈1.024s）。
     * TD 提交后要等当前帧号转一圈才轮到执行——等待必须覆盖
     * 一个完整周期（1.1s），否则必然超时。用 10ms 步进轮询。 */
    for (int i = 0; i < 110; i++) {          /* 110 × 10ms = 1.1s */
        if (!(td->ctrl_status & TD_CTRL_ACTIVE)) {
            /* 完成——检查是否带错误标志 */
            if (td->ctrl_status & (TD_CTRL_STALL | TD_CTRL_DBUF | TD_CTRL_BABBLE |
                                   TD_CTRL_NAK | TD_CTRL_CRCTIMEO | TD_CTRL_BITSTUFF |
                                   TD_CTRL_LENERR))
                return false;
            return true;
        }
        uhci_delay_ms(10);
    }
    return false;   /* 超时 */
}

/* 同步批量传输（BOT）：CBW(OUT,DATA0) → 数据(DATA1 起) → CSW(IN)，
 * 一次挂链等完再恢复。qemu 的 max_len=(token>>21)+1，长度存 (长度-1)。 */
int usb_bulk_bot(usb_hc_t *hc, uint8_t dev_addr,
                         uint8_t ep_out, uint8_t ep_in,
                         const void *cbw, uint16_t cbw_len,
                         void *data, uint32_t data_len, uint8_t dir,
                         void *csw, uint16_t csw_len) {
    (void)hc;
    int saved_td_used = uhci_priv.td_used;
    uint32_t saved_el = schedule_qh->el_link;

    uhci_td_t *td_cbw = uhci_alloc_td();
    uhci_td_t *td_data = NULL;
    uhci_td_t *td_csw = uhci_alloc_td();
    if (!td_cbw || !td_csw) {
        uhci_priv.td_used = saved_td_used;
        return -1;
    }

    /* TD1: CBW → bulk OUT 端点（DATA0，dt=0） */
    uint32_t token_cbw = (dev_addr << 8) | (ep_out << 15) | (0 << 19)
                       | (((uint32_t)cbw_len - 1) << 21) | (TD_TOKEN_PID_OUT << 0);
    uhci_setup_td(td_cbw, 0, 0, token_cbw, (uint32_t)(uintptr_t)cbw);

    /* TD2: 数据 → bulk IN/OUT 端点（data_len > 0 时才有） */
    if (data_len > 0) {
        td_data = uhci_alloc_td();
        if (!td_data) {
            uhci_priv.td_used = saved_td_used;
            return -1;
        }
        uint32_t token_data = (dev_addr << 8)
                            | ((dir ? ep_in : ep_out) << 15) | (1 << 19)
                            | ((data_len - 1) << 21)
                            | ((dir ? TD_TOKEN_PID_IN : TD_TOKEN_PID_OUT) << 0);
        uhci_setup_td(td_data, 0, 0, token_data, (uint32_t)(uintptr_t)data);
    }

    /* TD3: CSW ← bulk IN 端点 */
    uint32_t token_csw = (dev_addr << 8) | (ep_in << 15) | (1 << 19)
                       | (((uint32_t)csw_len - 1) << 21) | (TD_TOKEN_PID_IN << 0);
    uhci_setup_td(td_csw, 0x01, TD_CTRL_IOC, token_csw, (uint32_t)(uintptr_t)csw);

    /* 链接：CBW → (DATA) → CSW → 终止（bit0=0 才有效） */
    if (td_data) {
        td_cbw->link = (uint32_t)(uintptr_t)td_data;
        td_data->link = (uint32_t)(uintptr_t)td_csw;
    } else {
        td_cbw->link = (uint32_t)(uintptr_t)td_csw;
    }

    /* 挂到调度 QH 垂直链 */
    schedule_qh->el_link = (uint32_t)(uintptr_t)td_cbw;

    bool ok = uhci_wait_td(td_cbw, 100000);

    schedule_qh->el_link = saved_el;
    uhci_priv.td_used = saved_td_used;
    if (!ok) {
        vga_write("[USB-BULK] fail cbw="); vga_write_hex(td_cbw->ctrl_status);
        if (td_data) { vga_write(" data="); vga_write_hex(td_data->ctrl_status); }
        vga_write(" csw="); vga_write_hex(td_csw->ctrl_status);
        vga_write(" frnum="); vga_write_hex(uhci_readw(UHCI_FRNUM));
        vga_write("\n");
    }
    return ok ? 0 : -1;
}

/*
 * 同步控制传输：SETUP → (DATA) → STATUS 三个 TD 串链，挂调度 QH
 * 轮询等待。枚举/配置/设备控制全靠它。简化：不按帧号分散。
 */
static int uhci_control_transfer(usb_hc_t *hc, uint8_t dev_addr,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 void *data, uint16_t wLength) {
    (void)hc;

    /* 只回收自己分配的 TD，别把中断传输（键盘/鼠标）的一起回收 */
    int saved_td_used = uhci_priv.td_used;
    /* QH 垂直链可能挂着中断 TD：保存并在结束时恢复，别打断轮询 */
    uint32_t saved_el = schedule_qh->el_link;

    /* setup 包 = 8 字节标准请求头（方向/类型/接收者 + 请求号 + 参数） */
    uint8_t setup[8] = {
        bmRequestType, bRequest,
        (uint8_t)(wValue & 0xFF), (uint8_t)(wValue >> 8),
        (uint8_t)(wIndex & 0xFF), (uint8_t)(wIndex >> 8),
        (uint8_t)(wLength & 0xFF), (uint8_t)(wLength >> 8)
    };

    /* TD：SETUP + STATUS 必须，DATA 仅 wLength>0 时有 */
    uhci_td_t *td_setup = uhci_alloc_td();
    uhci_td_t *td_data = NULL;
    uhci_td_t *td_status = uhci_alloc_td();
    if (!td_setup || !td_status) return -1;

    /* token：bit0-7 PID、bit8-14 设备地址、bit15 端点、bit21+ 数据长度 */
    /* ⚠️ qemu 的 max_len=(token>>21)+1，长度字段存 (长度-1)！
     * 写 8 会被算成 9，SETUP 包 9≠8 → do_token_setup 直接 STALL。 */
    uint32_t token_setup = (dev_addr << 8) | (0 << 15) | (0 << 19) | (7 << 21) | (TD_TOKEN_PID_SETUP << 0);
    uint32_t token_in   = (dev_addr << 8) | (0 << 15) | (1 << 19) | ((wLength - 1) << 21) | (TD_TOKEN_PID_IN << 0);
    uint32_t token_out  = (dev_addr << 8) | (0 << 15) | (1 << 19) | ((wLength - 1) << 21) | (TD_TOKEN_PID_OUT << 0);

    uhci_setup_td(td_setup, 0, 0, token_setup, (uint32_t)(uintptr_t)setup);

    /* 链接：有数据则 SETUP → DATA → STATUS，否则 SETUP → STATUS。
     * link 位 0：1=TD/0=QH；位 1：垂直结束标志。 */
    if (wLength > 0) {
        td_data = uhci_alloc_td();
        if (!td_data) return -1;
        if (bmRequestType & 0x80) {
            uhci_setup_td(td_data, 0, 0, token_in, (uint32_t)(uintptr_t)data);
        } else {
            uhci_setup_td(td_data, 0, 0, token_out, (uint32_t)(uintptr_t)data);
        }
        /* ⚠️ link 不能置 bit0=1：qemu is_valid 要求 bit0=0 才有效，
         * bit0=1 被当成链表结束，0x01 只用于终止符 */
        td_setup->link = (uint32_t)(uintptr_t)td_data;
        td_data->link = (uint32_t)(uintptr_t)td_status;
    } else {
        td_setup->link = (uint32_t)(uintptr_t)td_status;
    }

    /* STATUS TD：方向与数据阶段相反（无数据阶段用 IN）；
     * 置 IOC 产生完成中断，buffer=0（状态阶段只走握手包） */
    uint32_t token_status;
    if (bmRequestType & 0x80)
        token_status = (dev_addr << 8) | (0 << 15) | (1 << 19) | (0x7FF << 21) | (TD_TOKEN_PID_OUT << 0);
    else
        token_status = (dev_addr << 8) | (0 << 15) | (1 << 19) | (0x7FF << 21) | (TD_TOKEN_PID_IN << 0);
    uhci_setup_td(td_status, 0x01, TD_CTRL_IOC, token_status, 0);  /* link=0x01 终止链 */

    /* 挂到调度 QH 垂直链（qemu 每 1ms 沿 QH 遍历 TD 链） */
    schedule_qh->el_link = (uint32_t)(uintptr_t)td_setup;

    /* 轮询等完成（SETUP 完成即整链完成，硬件顺序处理） */
    bool ok = uhci_wait_td(td_setup, 100000);

    /* 恢复 QH 垂直链、回滚 TD 计数（只释放自己分配的） */
    schedule_qh->el_link = saved_el;
    uhci_priv.td_used = saved_td_used;
    if (!ok) {
        /* 打印各 TD 状态 + 帧号（排查帧周期/错误位用） */
        vga_write("[USB-CTL] fail st="); vga_write_hex(td_setup->ctrl_status);
        if (td_data) { vga_write(" data="); vga_write_hex(td_data->ctrl_status); }
        vga_write(" status="); vga_write_hex(td_status->ctrl_status);
        vga_write(" frnum="); vga_write_hex(uhci_readw(UHCI_FRNUM));
        vga_write("\n");
    }
    return ok ? 0 : -1;
}

/*
 * 中断传输（轮询式）：USB 协议层面就是主机定期轮询设备。
 * TD 挂调度 QH，usb_poll() 周期检查完成并调回调。简化：
 * 没按 bInterval 分散挂帧，全部走调度 QH。
 */
typedef struct {
    usb_urb_t *urb;     /* USB 请求块，包含参数和回调函数 */
    uhci_td_t *td;      /* 对应的传输描述符 */
    bool active;        /* 该中断传输是否仍在活跃状态 */
    int dt;             /* ⚠️ 修复（留意项 1）：DATA toggle——每次重提交交替 */
    usb_hc_t *hc;       /* 所属控制器（出错重提交时用） */
} uhci_int_urb_t;

/* 中断传输 URB 表，最多跟踪 4 个活跃的中断传输 */
static uhci_int_urb_t int_urbs[4];
static int int_urb_count = 0;   /* 当前中断传输计数 */

static int uhci_interrupt_transfer(usb_hc_t *hc, uint8_t dev_addr,
                                   uint8_t ep, uint8_t *buffer, uint16_t len,
                                   void (*callback)(usb_urb_t *)) {
    (void)hc;

    /* 优先复用同 (dev_addr, ep) 的已完成传输：回调每份报告都重提交，
     * 每次新分配的话 64 个 TD 的池很快耗尽 */
    for (int i = 0; i < int_urb_count; i++) {
        uhci_int_urb_t *iu = &int_urbs[i];
        if (!iu->active && iu->urb && iu->td &&
            iu->urb->dev_addr == dev_addr && iu->urb->endpoint == ep) {
            /* ⚠️ 修复（留意项 1）：DATA toggle 交替——设备端按报告序号
             * 交替 DATA0/DATA1，主机 TD 的 token bit19 必须同步，否则
             * 真机（严格校验 toggle）第二次传输即失败。 */
            iu->dt = !iu->dt;
            uint32_t token = (dev_addr << 8) | (ep << 15) | ((iu->dt & 1) << 19) | ((len - 1) << 21) | (TD_TOKEN_PID_IN << 0);
            /* ⚠️ 修复（留意项 2）：TD 链终止用 link=0x01（T 位），
             * link=0 会让硬件尝试从物理地址 0 读下一个 TD。 */
            uhci_setup_td(iu->td, 0x01, TD_CTRL_IOC, token, (uint32_t)(uintptr_t)buffer);
            schedule_qh->el_link = (uint32_t)(uintptr_t)iu->td;
            iu->urb->buffer = buffer;
            iu->urb->length = len;
            iu->urb->callback = callback;
            iu->active = true;
            return 0;
        }
    }

    /* 首次提交：新分配 TD + URB（最多同时跟踪 4 个） */
    if (int_urb_count >= 4) return -1;

    uhci_td_t *td = uhci_alloc_td();
    if (!td) return -1;

    uint32_t token = (dev_addr << 8) | (ep << 15) | (0 << 19) | ((len - 1) << 21) | (TD_TOKEN_PID_IN << 0);
    /* ⚠️ 修复（留意项 2）：link=0x01 终止（首提同样适用） */
    uhci_setup_td(td, 0x01, TD_CTRL_IOC, token, (uint32_t)(uintptr_t)buffer);

    /* 挂到调度 QH 垂直链（bit0=0，qemu is_valid 要求） */
    schedule_qh->el_link = (uint32_t)(uintptr_t)td;

    usb_urb_t *urb = (usb_urb_t*)pmm_alloc_page();
    if (!urb) {
        /* ⚠️ 修复（留意项 4）：URB 分配失败释放刚分配的 TD——原实现泄漏 */
        uhci_free_td(td);
        return -1;
    }
    urb->type = USB_URB_INTERRUPT;
    urb->dev_addr = dev_addr;
    urb->endpoint = ep;
    urb->direction = 1;      /* 1 = 设备到主机（IN） */
    urb->length = len;
    urb->buffer = buffer;
    urb->callback = callback;

    int_urbs[int_urb_count].urb = urb;
    int_urbs[int_urb_count].td = td;
    int_urbs[int_urb_count].active = true;
    int_urbs[int_urb_count].dt = 0;   /* 首提 DATA0 */
    int_urbs[int_urb_count].hc = hc;
    int_urb_count++;
    return 0;
}

/* usb_poll：周期调用，检查中断 TD 完成并调回调（键盘/鼠标靠它收数）。
 * 重提交由回调里做（见 usb_kb_callback），出错路径在这里直接补提。 */
void usb_poll(void) {
    for (int i = 0; i < int_urb_count; i++) {
        if (!int_urbs[i].active) continue;
        uhci_td_t *td = int_urbs[i].td;
        if (!(td->ctrl_status & TD_CTRL_ACTIVE)) {
            int_urbs[i].active = false;
            usb_urb_t *urb = int_urbs[i].urb;
            /* ⚠️ 修复（留意项 3）：检查 TD 错误位（STALL/BABBLE/CRC/
             * 位填充/长度错误/DBUF）——出错时不调用回调，避免把陈旧
             * 或损坏的数据交给上层。NAK 排除：中断 IN 无新数据时设备
             * NAK 是正常现象（该 TD 会保持 ACTIVE 重试，走到这里时
             * NAK 位通常不置位，保守起见仍排除）。 */
            uint32_t err = td->ctrl_status &
                (TD_CTRL_STALL | TD_CTRL_DBUF | TD_CTRL_BABBLE |
                 TD_CTRL_CRCTIMEO | TD_CTRL_BITSTUFF | TD_CTRL_LENERR);
            if (urb->callback && !err) {
                /* 实际传输长度在状态字位 16-26 */
                urb->length = (td->ctrl_status >> 16) & 0x7FF;
                urb->callback(urb);
            } else if (urb && err) {
                /* ⚠️ 修复：出错时不调回调（避免脏数据进上层），但
                 * 必须保持轮询——否则键盘/鼠标中断传输永久停摆
                 * （回调里才有重提交逻辑）。这里直接重新提交。 */
                usb_urb_t nu = {
                    .type = USB_URB_INTERRUPT,
                    .dev_addr = urb->dev_addr,
                    .endpoint = urb->endpoint,
                    .direction = urb->direction,
                    .length = urb->length,
                    .buffer = urb->buffer,
                    .callback = urb->callback,
                };
                usb_submit_urb(int_urbs[i].hc, &nu);
            }
        }
    }
}

/* URB 分发：核心层 → 控制器驱动的人口，按传输类型分派 */
static int uhci_submit_urb(usb_hc_t *hc, usb_urb_t *urb) {
    if (!hc) return -1;
    if (urb->type == USB_URB_CONTROL) {
        return uhci_control_transfer(hc, urb->dev_addr,
                                     urb->req_type, urb->request,
                                     urb->value, urb->index,
                                     urb->buffer, urb->length);
    } else if (urb->type == USB_URB_INTERRUPT) {
        return uhci_interrupt_transfer(hc, urb->dev_addr,
                                       urb->endpoint, urb->buffer,
                                       urb->length, urb->callback);
    }
    /* 批量传输（BULK）和等时传输（ISOCHRONOUS）暂不支持 */
    return -1;
}

/* 前向声明：enumerate_device 末尾对 hub 设备（class 0x09）递归扫描下游 */
static void usb_hub_scan(usb_hc_t *hc, uint8_t hub_addr);

/*
 * 设备枚举：读描述符前 8 字节（拿 bMaxPacketSize0）→ 设地址
 * → 读完整设备/配置描述符 → 设配置 → 注册设备。任一步失败即中止。
 */

/* 读描述符（GET_DESCRIPTOR 封装） */
static int get_descriptor(usb_hc_t *hc, uint8_t dev_addr,
                          uint8_t desc_type, uint8_t desc_index,
                          void *buf, uint16_t len) {
    return usb_control_transfer(hc, dev_addr,
                                0x80, USB_REQ_GET_DESCRIPTOR,
                                (desc_type << 8) | desc_index,
                                0, buf, len);
}

/* 设地址：请求发到旧地址（通常 0），设备 ACK 后才切换 */
static int set_address(usb_hc_t *hc, uint8_t old_addr, uint8_t new_addr) {
    return usb_control_transfer(hc, old_addr,
                                0x00, USB_REQ_SET_ADDRESS,
                                new_addr, 0, NULL, 0);
}

/* 枚举一个端口上的设备并注册（只选第一个有效配置） */
static void enumerate_device(usb_hc_t *hc, uint8_t port) {
    (void)port;    /* 当前实现未使用 port 参数 */

    /* 地址 0 读前 8 字节：拿 bMaxPacketSize0（偏移 7），后续通信的基础 */
    uint8_t buf[8];
    int ret = get_descriptor(hc, 0, USB_DESC_DEVICE, 0, buf, 8);
    if (ret < 0) {
        vga_write("[USB] Get device desc (8) failed.\n");
        return;
    }
    uint8_t max_packet = buf[7];    /* bMaxPacketSize0 偏移量 = 7 */
    (void)max_packet;    /* 本实现尚未使用此值进行后续通信 */

    /* 分配地址（1 起递增）并发送 SET_ADDRESS */
    uint8_t new_addr = uhci_priv.next_address++;
    if (new_addr == 0) new_addr = 1;  /* 地址 0 保留给默认状态 */
    ret = set_address(hc, 0, new_addr);
    if (ret < 0) {
        vga_write("[USB] Set address failed.\n");
        return;
    }

    /* 设备需要一小段时间来稳定状态（复位恢复时间），规范建议至少 2ms */
    uhci_delay_ms(2);

    /* 用新地址读完整设备描述符（18 字节） */
    usb_device_descriptor_t dev_desc;
    ret = get_descriptor(hc, new_addr, USB_DESC_DEVICE, 0, &dev_desc, sizeof(dev_desc));
    if (ret < 0) {
        vga_write("[USB] Get full device desc failed.\n");
        return;
    }

    /* 读配置：先读 9 字节头拿 wTotalLength，再读全量
     * （配置 = 头 + 若干接口 + 端点描述符） */
    uint8_t config_buf[256];
    ret = get_descriptor(hc, new_addr, USB_DESC_CONFIGURATION, 0, config_buf, 9);
    if (ret < 0) {
        vga_write("[USB] Get config header failed.\n");
        return;
    }
    usb_config_descriptor_t *cfg = (usb_config_descriptor_t*)config_buf;
    uint16_t total_len = cfg->wTotalLength;
    if (total_len > sizeof(config_buf)) total_len = sizeof(config_buf);
    ret = get_descriptor(hc, new_addr, USB_DESC_CONFIGURATION, 0, config_buf, total_len);
    if (ret < 0) {
        vga_write("[USB] Get full config failed.\n");
        return;
    }

    /* 分配设备结构体、注册到全局链表（上层驱动遍历认领） */
    usb_device_t *dev = (usb_device_t*)pmm_alloc_page();
    if (!dev) {
        vga_write("[USB] Out of memory for device.\n");
        return;
    }
    dev->address = new_addr;
    dev->hc = hc;
    memcpy(&dev->dev_desc, &dev_desc, sizeof(dev_desc));
    dev->num_configs = dev_desc.bNumConfigurations;
    dev->next = NULL;
    usb_register_device(dev);

    /* 设配置：进入 Configured 状态后非端点 0 的传输才可用 */
    if (cfg->bConfigurationValue) {
        ret = usb_control_transfer(hc, new_addr,
                                   0x00, USB_REQ_SET_CONFIGURATION,
                                   cfg->bConfigurationValue, 0, NULL, 0);
        if (ret < 0)
            vga_write("[USB] Set config failed.\n");
        else
            vga_write("[USB] Configuration set.\n");
    }

    /* 输出设备识别信息供调试 */
    vga_write("[USB] Device enumerated: VID=");
    vga_write_hex(dev_desc.idVendor);
    vga_write(" PID=");
    vga_write_hex(dev_desc.idProduct);
    vga_write("\n");

    /* ⚠️ 新增：HUB 设备（class 0x09）→ 递归扫描下游端口。
     * 真机键鼠/U 盘常经 hub 连接；QEMU 在 UHCI(1.1) 下用虚拟 hub
     * 桥接 USB2 设备（usb-storage 挂在 hub 下游，无此支持枚举不到）。 */
    if (dev_desc.bDeviceClass == 0x09) {
        vga_write("[USB] Hub detected, scanning downstream ports...\n");
        usb_hub_scan(hc, new_addr);
    }
}

/* ---- USB HUB 支持（class 0x09） ---- */

/* 读 hub 端口数（HUB 描述符 bNbrPorts） */
static uint8_t usb_hub_port_count(usb_hc_t *hc, uint8_t hub_addr) {
    uint8_t buf[8];
    int ret = usb_control_transfer(hc, hub_addr,
                                   0x80, USB_REQ_GET_DESCRIPTOR,
                                   (0x29 << 8) | 0,   /* HUB 描述符 */
                                   0, buf, sizeof(buf));
    if (ret < 0) return 0;
    return buf[2];   /* bNbrPorts */
}

/* 扫 hub 下游端口：CCS 判有无设备 → 供电 → 复位 → 枚举。
 * 下游从地址 0 开始（hub 把地址 0 请求转发到该端口）；
 * 下游若是 hub，enumerate_device 内部递归。 */
static void usb_hub_scan(usb_hc_t *hc, uint8_t hub_addr) {
    uint8_t nports = usb_hub_port_count(hc, hub_addr);
    if (nports == 0 || nports > 16) nports = 4;   /* 描述符失败时兜底 */
    vga_write("[USB] Hub with ");
    vga_write_hex(nports);
    vga_write(" ports, scanning downstream...\n");

    for (uint8_t port = 1; port <= nports; port++) {
        uint8_t st[4] = {0, 0, 0, 0};
        /* GET_PORT_STATUS：class 请求，wIndex = 端口号（从 1 开始） */
        int ret = usb_control_transfer(hc, hub_addr, 0xA3, 0x00 /* GET_STATUS */,
                                       0, port, st, sizeof(st));
        if (ret < 0) continue;
        uint16_t status = (uint16_t)(st[0] | (st[1] << 8));
        if (!(status & 0x0001)) continue;   /* CCS：无设备连接 */

        vga_write("[USB] Hub port ");
        vga_write_hex(port);
        vga_write(" has device, resetting...\n");

        /* PORT_POWER (8)：保证端口供电（真机需要；QEMU 默认已供电） */
        usb_control_transfer(hc, hub_addr, 0x23, 0x03 /* SET_FEATURE */,
                             8, port, NULL, 0);
        /* PORT_RESET (4)：复位下游设备 */
        usb_control_transfer(hc, hub_addr, 0x23, 0x03 /* SET_FEATURE */,
                             4, port, NULL, 0);
        uhci_delay_ms(50);   /* 复位稳定时间 */

        /* 枚举下游设备（从地址 0 开始；hub 转发地址 0 请求到该端口） */
        enumerate_device(hc, port);
    }
}

/*
 * 控制器初始化：保存 BAR0 → 全局复位 → 分配帧列表/调度 QH/TD 池
 * → 写 FRBASE → 使能中断 → 启动（RS|CF）→ 扫端口枚举。
 * 内存结构必须先就位再启动：RS=1 后硬件立即遍历帧列表。
 */
static void uhci_init(usb_hc_t *hc) {
    /* 保存 I/O 基地址（PCI BAR0） */
    uhci_priv.io_base = hc->base_addr;
    vga_write("[UHCI] Init at I/O 0x");
    vga_write_hex(uhci_priv.io_base);
    vga_write("\n");

    /* 全局复位：总线发 ≥10ms SE0，设备回到默认地址 0 */
    uhci_writew(UHCI_CMD, UHCI_CMD_GRESET);
    uhci_delay_ms(10);   /* 全局复位需保持 ≥10ms */
    uhci_writew(UHCI_CMD, 0);
    uhci_delay_ms(1);

    /* 分配帧列表：1024 项 × 4B = 4KB，硬件 DMA 读 */
    uhci_priv.frame_list = (uint32_t*)alloc_phys_pages(1, &uhci_priv.frame_list_phys);
    if (!uhci_priv.frame_list) {
        vga_write("[UHCI] Frame list alloc failed.\n");
        return;
    }

    /* 分配调度 QH，所有帧列表项指向它（QH 指针 = 地址|0x02）：
     * qemu 要求帧列表指向 QH 而非 TD，控制传输才能组装成一次事务 */
    schedule_qh = (uhci_qh_t*)alloc_phys_pages(1, &schedule_qh_phys);
    if (!schedule_qh) {
        vga_write("[UHCI] Schedule QH alloc failed.\n");
        return;
    }
    schedule_qh->link = 0x01;      /* 水平链终止 */
    schedule_qh->el_link = 0x01;   /* 垂直链为空 */
    for (int i=0; i<FRAME_LIST_SIZE; i++)
        uhci_priv.frame_list[i] = schedule_qh_phys | 0x02;

    /* 预分配 TD 池（DMA 要物理连续，预分配免运行时失败） */
    int td_pages = (MAX_TDS * sizeof(uhci_td_t) + 4095) / 4096;
    uhci_priv.td_pool = (uhci_td_t*)alloc_phys_pages(td_pages, &uhci_priv.td_pool_phys);
    if (!uhci_priv.td_pool) {
        vga_write("[UHCI] TD pool alloc failed.\n");
        return;
    }
    uhci_priv.td_used = 0;
    uhci_priv.next_address = 1;  /* USB 设备地址从 1 开始分配 */

    /* 写 FRBASE 指向帧列表 */
    uhci_writel(UHCI_FRBASE, uhci_priv.frame_list_phys);

    /* 使能 USBINT（IOC 位 TD 完成触发） */
    uhci_writew(UHCI_INTR, 0x0001); /* 使能短包/传输完成中断 */

    /* 启动：RS=1 跑帧列表（1ms/帧），CF=1 标记配置完成 */
    uhci_writew(UHCI_CMD, UHCI_CMD_RS | UHCI_CMD_CF);
    uhci_priv.running = true;


    /* 扫两个端口：CCS 有设备 → 复位 → 使能 → 枚举 */
    for (int port=0; port<2; port++) {
        uint16_t status = uhci_readw(UHCI_PORT1 + port*2);
        if (status & UHCI_PORT_CCS) {
            vga_write("[UHCI] Device on port ");
            vga_write_hex(port);
            vga_write("\n");

            /* 端口复位：写 1 到 RESET 位（bit9）发起复位。
             * ⚠️ 注意：qemu 的端口位定义与经典 UHCI 不同：
             *   RESET=bit9、RD=bit6、SUSPEND=bit12、bit7 是保留位。
             * 内核旧定义用 bit5（RD）当复位位，qemu 根本不认，
             * 设备从未被复位过。复位信号至少持续 10ms。 */
            uhci_writew(UHCI_PORT1 + port*2, status | (1 << 9));
            uhci_delay_ms(10);   /* 端口复位需保持 ≥10ms */

            /* 使能端口：写 PE（bit2 = qemu 的 EN） */
            uhci_writew(UHCI_PORT1 + port*2, UHCI_PORT_PE);
            uhci_delay_ms(2);


            enumerate_device(hc, port);
        }
    }
}

/*
 * USB 核心层：控制器列表 + 设备链表 + URB 提交入口。
 * 分层：类驱动（HID/MSC）→ 核心层 → 控制器驱动（本文件）。
 */

/* 已发现控制器数组（PCI 扫描填充） */
static usb_hc_t hc_list[8];
static int hc_count = 0;       /* 已发现的主机控制器数量 */

/* 已枚举设备链表头 */
static usb_device_t *device_list = NULL;

/* 头插法加入设备链表 */
void usb_register_device(usb_device_t *dev) {
    dev->next = device_list;
    device_list = dev;
}

/* 返回设备链表头（类驱动遍历认领设备） */
usb_device_t* usb_get_device_list(void) {
    return device_list;
}

/* 注册控制器（PCI 扫描调用） */
void usb_register_hc(usb_hc_t *hc) {
    if (hc_count < 8) hc_list[hc_count++] = *hc;
}

/* 控制传输封装：参数即 USB 标准请求字段，内部构造 URB 提交 */
int usb_control_transfer(usb_hc_t *hc, uint8_t dev_addr,
                         uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex,
                         void *data, uint16_t wLength) {
    if (!hc || !hc->submit_urb) return -1;
    usb_urb_t urb = {
        .type = USB_URB_CONTROL,
        .dev_addr = dev_addr,
        .endpoint = 0,                            /* 控制传输始终使用端点 0 */
        .direction = (bmRequestType & 0x80) ? 1 : 0,  /* 1=IN, 0=OUT */
        .length = wLength,
        .buffer = data,
        .req_type = bmRequestType,
        .request = bRequest,
        .value = wValue,
        .index = wIndex,
        .callback = NULL,                         /* 同步传输无需回调 */
    };
    return hc->submit_urb(hc, &urb);
}

/* 提交构造好的 URB（中断/批量等非控制传输用） */
int usb_submit_urb(usb_hc_t *hc, usb_urb_t *urb) {
    if (!hc || !hc->submit_urb) return -1;
    return hc->submit_urb(hc, urb);
}

/*
 * PCI 扫描找 UHCI：class=0x0C、subclass=0x03，I/O 基址扫全部
 * 6 个 BAR 取第一个非零值。
 */
static void probe_controllers(void) {
    for (int bus=0; bus<256; bus++) {
        for (int slot=0; slot<32; slot++) {
            for (int func=0; func<8; func++) {
                /* VID=0xFFFF 是空槽：func0 空则整个设备不存在 */
                uint32_t vid = pci_read_config(bus, slot, func, 0) & 0xFFFF;
                if (vid == 0xFFFF) {
                    if (func == 0) break;
                    else continue;
                }

                /* class_rev：bit31-24 类码、bit23-16 子类 */
                uint32_t class_rev = pci_read_config(bus, slot, func, 0x08);
                uint8_t class = (class_rev >> 24) & 0xFF;
                uint8_t subclass = (class_rev >> 16) & 0xFF;

                if (class != 0x0C || subclass != 0x03) continue;   /* 非 USB 主机控制器 */

                /* I/O 基址扫全部 6 个 BAR 取第一个非零值：
                 * qemu 的 piix3-usb-uhci 用 BAR4，只读 BAR0 会拿 0 */
                uint32_t bar = 0;
                for (int b = 0; b < 6; b++) {
                    uint32_t v = pci_read_config(bus, slot, func, 0x10 + b * 4) & ~0x0F;
                    if (v) { bar = v; break; }
                }
                if (!bar) continue;   /* 没有有效的 I/O BAR */

                usb_hc_t hc = {0};
                hc.bus = bus; hc.slot = slot; hc.func = func;
                hc.base_addr = bar;
                hc.type = USB_HC_UHCI;
                hc.init = uhci_init;
                hc.submit_urb = uhci_submit_urb;
                usb_register_hc(&hc);
                vga_write("[USB] UHCI found at ");
                vga_write_hex(bar);
                vga_write("\n");
            }
        }
    }
}

/* USB 子系统入口：扫控制器 → 逐个初始化（内含枚举）→ 子模块认领 */
void usb_init(void) {
    vga_write("[USB] Initializing...\n");

    probe_controllers();
    if (hc_count == 0) {
        vga_write("[USB] No UHCI controller.\n");
        return;
    }

    for (int i=0; i<hc_count; i++) {
        if (hc_list[i].init)
            hc_list[i].init(&hc_list[i]);
    }

    /* 枚举在 uhci_init 里完成，这里让子模块认领设备 */
    usb_hid_init();
    usb_mass_storage_init();

    vga_write("[USB] Init done.\n");
}

/*
 * 在配置描述符里找 HID 接口（bInterfaceClass=0x03），
 * 顺带收集它的 IN/OUT 端点。键盘/鼠标驱动共用。
 */
bool find_hid_interface(usb_hc_t *hc, uint8_t dev_addr,
                        const uint8_t *config_data, uint16_t total_len,
                        usb_interface_descriptor_t **out_iface,
                        usb_endpoint_descriptor_t **out_ep_in,
                        usb_endpoint_descriptor_t **out_ep_out) {
    (void)hc; (void)dev_addr;    /* 参数暂未使用，保留接口兼容性 */

    uint16_t pos = 0;
    while (pos < total_len) {
        uint8_t len = config_data[pos];      /* 当前描述符长度 */
        if (len == 0) break;                 /* 零长度 = 数据损坏 */
        uint8_t type = config_data[pos+1];   /* 描述符类型 */

        if (type == USB_DESC_INTERFACE) {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t*)&config_data[pos];
            if (iface->bInterfaceClass == 0x03) {   /* HID 类 */
                *out_iface = iface;
                pos += len;

                /* 收集该接口下的 IN/OUT 端点 */
                while (pos < total_len) {
                    uint8_t l = config_data[pos];
                    if (l == 0) break;
                    uint8_t t = config_data[pos+1];
                    if (t == USB_DESC_ENDPOINT) {
                        /* bit7=1 为 IN 端点，bit7=0 为 OUT */
                        usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t*)&config_data[pos];
                        if ((ep->bEndpointAddress & 0x80) && !*out_ep_in)
                            *out_ep_in = ep;     /* 记录第一个 IN 端点 */
                        else if (!(ep->bEndpointAddress & 0x80) && !*out_ep_out)
                            *out_ep_out = ep;    /* 记录第一个 OUT 端点 */
                    } else if (t == USB_DESC_INTERFACE) {
                        break;  /* 遇到下一个接口就停 */
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
