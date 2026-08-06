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

/* ================================================================
 *  第1章: UHCI 控制器寄存器定义
 *
 *  UHCI（Universal Host Controller Interface）是 Intel 制定的
 *  USB 1.x 主机控制器标准接口规范。它负责管理 USB 总线上的所有
 *  数据传输，是 USB 主机端最底层的硬件控制器。
 *
 *  UHCI 控制器通过 I/O 端口（而非内存映射）对外暴露一组寄存器。
 *  驱动程序通过读写这些寄存器来控制 UHCI 的行为：
 *    - 启动/停止 USB 总线（CMD 寄存器的 RS 位）
 *    - 管理帧列表（Frame List），这是 UHCI 最核心的数据结构
 *    - 检测端口状态变化（设备插入/拔出、复位、速度检测等）
 *    - 处理中断和错误状态
 *
 *  UHCI 与 OHCI 的区别：
 *    - UHCI（Intel）将大部分调度逻辑交给软件，硬件相对简单
 *    - OHCI（Compaq/Microsoft）硬件承担更多调度工作
 *    - 两者最终都被 EHCI（USB 2.0）和 xHCI（USB 3.x）取代
 *
 *  寄存器偏移地址（相对于 I/O Base Address）：
 * ================================================================ */

/*
 *  0x00 — UHCI_CMD: 命令寄存器（Command Register，16 位）
 *  控制 UHCI 控制器的运行状态。这是最重要的控制寄存器，
 *  驱动程序通过它执行全局复位、启动/停止帧列表处理、设置
 *  全局配置标志等操作。
 */
#define UHCI_CMD       0x00

/*
 *  0x02 — UHCI_STS: 状态寄存器（Status Register，16 位）
 *  反映控制器的当前状态，包括 USB 中断、错误、主机控制暂停
 *  等状态位。驱动程序读取此寄存器判断发生了什么事件，写入
 *  写1清零对应位。
 */
#define UHCI_STS       0x02

/*
 *  0x04 — UHCI_INTR: 中断使能寄存器（Interrupt Enable，16 位）
 *  控制哪些状态事件可以触发中断送达 CPU。例如 USBINT（传输
 *  完成中断）和 USBERR（传输错误中断）的中断使能位。
 */
#define UHCI_INTR      0x04

/*
 *  0x06 — UHCI_FRNUM: 帧号寄存器（Frame Number Register，16 位）
 *  存储当前正在处理的帧编号（0~1023）。UHCI 以 1ms 为单位
 *  分割时间，每毫秒处理一个帧。驱动程序可以读取此寄存器
 *  获取当前帧号，也可以写入以指定起始帧号。
 */
#define UHCI_FRNUM     0x06

/*
 *  0x08 — UHCI_FRBASE: 帧列表基址寄存器（Frame Base Address，32 位）
 *  指向帧列表（Frame List）的物理基地址。帧列表是一个包含
 *  1024 个 32 位指针的数组，每个指针指向一个队列头（QH）
 *  或传输描述符（TD），代表着该毫秒内要执行的所有传输任务。
 *  该寄存器必须在 UHCI 开始运行前设定。
 */
#define UHCI_FRBASE    0x08

/*
 *  0x0C — UHCI_SOF: SOF 调节寄存器（Start of Frame Modify，16 位）
 *  允许微调帧起始包（Start-of-Frame, SOF）的定时。SOF 是
 *  USB 总线上的周期性信号，以 1ms 间隔发送，用于同步所有
 *  USB 设备的时间基准。
 */
#define UHCI_SOF       0x0C

/*
 *  0x10 — UHCI_PORT1: 端口 1 状态与控制寄存器（16 位）
 *  0x12 — UHCI_PORT2: 端口 2 状态与控制寄存器（16 位）
 *  每个 UHCI 控制器最多支持两个 USB 端口。通过这些寄存器，
 *  驱动程序可以检测设备连接/断开、启用/禁用端口、复位端口、
 *  判断设备速度（全速/低速）等。
 */
#define UHCI_PORT1     0x10
#define UHCI_PORT2     0x12


/*
 *  UHCI 命令寄存器（CMD）中各控制位的定义
 */

/* 位 0 — RS (Run/Stop): 写 1 启动帧列表处理，主机开始
 * 在 USB 总线上发送 SOF 包并执行传播描述符链。写 0 停止。*/
#define UHCI_CMD_RS     (1 << 0)

/* 位 1 — HCRESET (Host Controller Reset): 写 1 对控制器
 * 执行硬件复位（软件复位），复位后所有寄存器恢复默认值。*/
#define UHCI_CMD_HC     (1 << 1)

/* 位 2 — GRESET (Global Reset): 写 1 在 USB 总线上发送
 * 全局复位信号（至少 10ms），所有设备被复位到默认地址 0。*/
#define UHCI_CMD_GRESET (1 << 2)

/* 位 6 — CF (Configure Flag): 必须写 1 表示驱动程序已经
 * 完成配置。在 CF=1 之前，所有端口的状态改变都不会产生中断。
 * 这是一个"配置完成"的手续位。*/
#define UHCI_CMD_CF     (1 << 6)


/*
 *  UHCI 状态寄存器（STS）中的状态位
 */

/* 位 0 — USBINT: USB 中断，表示一个传输已经完成（带有 IOC
 * 位的 TD 被成功执行）。写 1 清零。*/
#define UHCI_STS_USBINT (1 << 0)

/* 位 1 — USBERR: USB 错误中断，表示发生了传输错误
 * （如 STALL、CRC 错误、Babble 等）。写 1 清零。*/
#define UHCI_STS_USBERR (1 << 1)


/*
 *  UHCI 端口状态与控制寄存器的位定义
 *  每一位都对应端口的一个硬件信号或状态。
 */

/* 位 0 — CCS (Current Connect Status): 当前连接状态。
 * 1 表示设备已连接，0 表示未连接。*/
#define UHCI_PORT_CCS   (1 << 0)

/* 位 1 — CSC (Connect Status Change): 连接状态变化标志。
 * 当设备的连接或断开事件发生时，硬件将此位置 1。软件需
 * 写 1 清零以确认收到该变化通知。*/
#define UHCI_PORT_CSC   (1 << 1)

/* 位 2 — PE (Port Enable): 端口使能位。写 1 使能端口，
 * 允许端口进行数据传输。硬件或错误条件可以自动清零此位
 * 来禁用端口。*/
#define UHCI_PORT_PE    (1 << 2)

/* 位 3 — PEC (Port Enable Change): 端口使能变化标志。
 * 当 PE 位因硬件或错误条件被清零时，此位置 1。写 1 清零。*/
#define UHCI_PORT_PEC   (1 << 3)

/* 位 4 — LSDA (Low Speed Device Attached): 低速设备连接
 * 指示。1 表示连接的是低速（1.5 Mbps）设备，0 表示全速
 * （12 Mbps）设备。USB 1.x 只有这两种速度。*/
#define UHCI_PORT_LSDA  (1 << 4)

/* 位 5 — RD (Reset Detect): 端口复位位/复位完成标志。
 * 写 1 发起端口复位（至少 10ms 的低电平信号），硬件在
 * 复位完成后自动清零此位。*/
#define UHCI_PORT_RD    (1 << 5)

/* 位 7 — SUSP (Suspend): 挂起位。写 1 使端口进入挂起
 * 状态以节省功耗，设备将进入低功耗模式。*/
#define UHCI_PORT_SUSP  (1 << 7)


/*
 *  帧列表与 TD 池的大小常量
 *  FRAME_LIST_SIZE = 1024: 每个 UHCI 控制器维护一张
 *  1024 项的帧列表。USB 帧周期为 1ms，因此覆盖约 1 秒
 *  的时间范围。每帧通过帧号（0-1023）循环索引。
 *
 *  MAX_TDS = 64: 预分配的传输描述符数量。这是软件资源
 *  池的大小而非硬件限制。每个 TD 固定 16 字节对齐到
 *  16 字节边界（硬件要求）。
 */
#define FRAME_LIST_SIZE 1024
#define MAX_TDS         64

/* ================================================================
 *  第2章: 传输描述符（TD — Transfer Descriptor）
 *
 *  TD 是 UHCI 中最基本的数据传输单元。每个 TD 描述了一次
 *  USB 总线事务（transaction）：向谁发送数据、发送什么类型
 *  的包（SETUP/IN/OUT）、数据缓冲区在哪、传输完成后是否
 *  产生中断等。
 *
 *  TD 结构体必须 16 字节对齐，这正是 __attribute__((aligned(16)))
 *  的作用。硬件要求 TD 的物理地址最低 4 位为 0。
 *
 *  TD 的四个字段（每个 32 位，共 16 字节）：
 *    1. link: 垂直链接指针。指向下一个 TD 或 QH，形成链表。
 *       最低位（位 0）= 1 表示下一个是 TD，= 0 表示 QH。
 *    2. ctrl_status: 控制/状态字。由软件设置控制位，硬件
 *       在执行过程中更新状态位（如 ACTIVE、STALL 等）。
 *    3. token: 令牌包定义。包含 PID（包标识符，如 SETUP/
 *       IN/OUT）、设备地址、端点号、数据长度等信息。
 *    4. buffer_ptr: 数据缓冲区物理地址。指向要发送或接收
 *       数据的内存区域。
 *
 *  工作原理：UHCI 硬件通过帧列表找到每帧的第一个 TD/QH，
 *  然后沿着 TD 之间的链接指针遍历整个链表。每遇到一个 TD，
 *  控制器就发起一次 USB 总线事务。当 TD 带有 VF（Vertical
 *  Flag）位标记时，表示垂直遍历结束，返回到帧列表取下一项。
 *
 *  注意：本驱动为主机控制器驱动程序，代码中的 TD 是软件构造的
 *  数据结构，与 USB 协议包（token packet、data packet、handshake
 *  packet）是两个不同层次的概念。TD 是 UHCI 硬件调度的单位，
 *  而 USB 协议包是 TD 所发起的总线事务中具体传输的数据格式。
 * ================================================================ */
typedef struct uhci_td {
    uint32_t link;           /* 链接指针：指向下一个 TD/QH 的物理地址
                                （位 1-31），位 0 表示类型（1=TD, 0=QH），
                                位 1 表示是否垂直结束（VF 标志）。*/
    uint32_t ctrl_status;    /* 控制/状态：高位是状态位（软件写入后由硬件
                                更新），低位是控制位（如 IOC、SPD 等）。
                                初始由软件写入，硬件执行时修改状态部分。*/
    uint32_t token;          /* 令牌字：定义了总线上传输的令牌包参数：
                                PID（包类型）、设备地址、端点号、数据长度。*/
    uint32_t buffer_ptr;     /* 数据缓冲区物理地址：指向该次传输要使用的
                                内存缓冲区，USB 控制器通过 DMA 直接访问。*/
} __attribute__((aligned(16))) uhci_td_t;

/*
 *  UHCI 队列头（Queue Head，8 字节）。
 *  qemu 的 UHCI 实现要求帧列表指向 QH（bit0=0, bit1=1），
 *  TD 链从 QH 的 el_link 垂直挂下——直接把 TD 挂帧列表
 *  （q=NULL）会导致控制传输无法组装成 setup→data→status
 *  一次事务，设备必然 STALL。
 */
typedef struct __attribute__((aligned(16))) {
    uint32_t link;      /* 水平链接：下一个 QH，或 0x01 终止 */
    uint32_t el_link;   /* 垂直链接：第一个 TD（bit0=0），0x01=空 */
} uhci_qh_t;


/*
 *  TD 控制/状态字（ctrl_status 字段）中各位的定义
 *  注意：控制位由软件设置，状态位由硬件更新
 */

/* 位 23 — ACTIVE: 活跃标志。软件置 1 向硬件表示此 TD
 * 待处理。硬件处理完成后清零此位。如果传输出错，硬件也
 * 会清零 ACTIVE 并在对应错误位置 1。*/
#define TD_CTRL_ACTIVE   (1 << 23)

/* 位 22 — STALL: 设备返回 STALL 握手包。表示设备不支持
 * 该请求或端点已挂起（halted）。这是 USB 协议级别的错误。*/
#define TD_CTRL_STALL    (1 << 22)

/* 位 21 — DBUF (Data Buffer Error): 数据缓冲区错误。
 * 当硬件在 DMA 访问数据缓冲区时遇到问题（如地址不可访问）
 * 时置位。通常是软件 bug。*/
#define TD_CTRL_DBUF     (1 << 21)

/* 位 20 — BABBLE: 设备发送了超过预期长度的数据（babbling）。
 * 全速设备最多可发送 1023 字节/事务，超过即产生 babble。*/
#define TD_CTRL_BABBLE   (1 << 20)

/* 位 19 — NAK: 设备返回 NAK 握手包。NAK 不是错误，表示
 * 设备暂时无法处理数据（例如数据未准备好）。在中断传输中，
 * NAK 表示设备没有新数据。UHCI 会自动重试 NAK 的 TD。*/
#define TD_CTRL_NAK      (1 << 19)

/* 位 18 — CRC/Timeout: CRC 校验错误或总线超时。
 * 当数据包 CRC 校验失败或设备无响应时置位。*/
#define TD_CTRL_CRCTIMEO (1 << 18)

/* 位 17 — BITSTUFF: 位填充错误。USB 总线使用 NRZI 编码
 * 和位填充机制，当接收检测到填充违规时置位。*/
#define TD_CTRL_BITSTUFF (1 << 17)

/* 位 16 — LENERR (Length Error): 接收的数据包实际长度
 * 与令牌中指定的期望长度不匹配时置位。*/
#define TD_CTRL_LENERR   (1 << 16)

/*
 *  TD 控制位（软件主动设置的位）
 */

/* 位 7 — IOC (Interrupt on Complete): 完成时产生中断。
 * 设置后，硬件执行完此 TD 后会触发 USBINT 中断。通常
 * 用于控制传输的状态阶段或任何需要软件介入的点。*/
#define TD_CTRL_IOC      (1 << 7)

/* 位 8 — SPD (Short Packet Detect): 短包检测使能。
 * 如果设置，硬件在收到短包（长度小于期望长度）时视为此
 * TD 完成。这对批量传输的短包检测很有用。*/
#define TD_CTRL_SPD      (1 << 8)


/*
 *  UHCI 令牌包 PID（Token Packet Identifier）
 *  这是 USB 总线事务的"类型"标识。
 *
 *  SETUP（0x2D）：控制传输的设置阶段，发送 8 字节
 *  的标准 USB 请求（bmRequestType, bRequest, wValue,
 *  wIndex, wLength）。
 *
 *  OUT（0xE1）：主机到设备的单向数据传输。
 *
 *  IN（0x69）：设备到主机的单向数据传输。
 *
 *  注意：这些 PID 值是 USB 规范中定义的标准值，通过
 *  PID 位的反转（校验）来保证数据完整性：
 *  SETUP = b2D(16) = 10101101b，比特反转校验为 01010010b
 *  实际上 USB 令牌包包含 PID（4 位）和其反码（4 位）。
 *  宏中给出的是完整字节值。*/
#define TD_TOKEN_PID_SETUP  0x2D
#define TD_TOKEN_PID_OUT    0xE1
#define TD_TOKEN_PID_IN     0x69

/* ================================================================
 *  第3章: UHCI 私有数据结构
 *
 *  每个 UHCI 控制器实例对应一个 uhci_private_t 结构体。
 *  它保存了该控制器的所有运行时状态信息。
 *
 *  字段说明：
 *    io_base: PCI 配置空间中 BAR0 给出的 I/O 基地址。
 *      所有寄存器访问都基于此偏移。
 *    frame_list: 帧列表的虚拟地址（内核映射）。
 *      这是一个 1024 × 4 = 4KB 的数组，每个元素指向
 *      一个 TD 链的起始地址。
 *    frame_list_phys: 帧列表的物理地址。必须写入
 *      FRBASE 寄存器供硬件 DMA 访问。
 *    td_pool: TD 池的虚拟地址。预分配的 TD 数组，
 *      用于控制传输和中断传输。
 *    td_pool_phys: TD 池的物理地址。当 TD 被链接到
 *      帧列表时，链接指针需要使用物理地址。
 *    td_used: 当前已使用的 TD 数量。每次控制传输后
 *      重置为 0（简化实现），实际驱动程序应管理空闲列表。
 *    running: 硬件是否正在运行（RS 位状态）。
 *    next_address: 下一次设备枚举时使用的 USB 地址。
 *      USB 地址范围是 1~127，每成功枚举一个设备递增。
 *
 *  注意：本驱动假设只有一个 UHCI 控制器实例。多控制器
 *  场景需要扩展为结构体数组或动态分配。
 * ================================================================ */
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

/* ================================================================
 *  第4章: 物理页内存分配辅助函数
 *
 *  USB 控制器通过 DMA（Direct Memory Access）直接访问内存，
 *  因此帧列表和 TD 池必须位于物理地址连续的内存中，且必须
 *  使用物理地址（而非虚拟地址）与硬件通信。
 *
 *  alloc_phys_pages: 分配 count 个连续的 4KB 物理页，
 *  返回其虚拟地址（用于 CPU 访问）并通过 phys 传出
 *  物理地址（用于硬件 DMA）。
 *
 *  注意：分配连续物理页在长时间运行后可能失败（物理内存
 *  碎片化），实际系统应启动时预留。此处连续检查确保相邻。
 *  失败时已分配的页会被回滚释放。
 * ================================================================ */
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

/* ================================================================
 *  第5章: UHCI 寄存器读写封装
 *
 *  UHCI 使用 I/O 端口（in/out 指令）而非内存映射来访问
 *  寄存器。这些内联函数封装了对 inw/outw/inl/outl 的调用，
 *  自动将私有数据中的 io_base 加到寄存器偏移上。
 *
 *  为什么使用 16 位和 32 位访问？
 *    - CMD、STS、INTR、FRNUM 等寄存器是 16 位宽 → 使用 word 访问
 *    - FRBASE 寄存器是 32 位宽（需要 32 位物理地址） → 使用 long 访问
 *    - 端口寄存器是 16 位宽 → 使用 word 访问
 *
 *  这些函数声明为 static inline 以减少函数调用开销，
 *  因为它们可能被频繁调用（尤其是在轮询等待 TD 完成时）。
 * ================================================================ */
static inline void uhci_writew(uint16_t reg, uint16_t val) {
    outw(uhci_priv.io_base + reg, val);    /* 向指定寄存器写入 16 位值 */
}
static inline uint16_t uhci_readw(uint16_t reg) {
    return inw(uhci_priv.io_base + reg);   /* 从指定寄存器读取 16 位值 */
}
static inline void uhci_writel(uint16_t reg, uint32_t val) {
    outl(uhci_priv.io_base + reg, val);    /* 向指定寄存器写入 32 位值 */
}
static inline uint32_t uhci_readl(uint16_t reg) {
    return inl(uhci_priv.io_base + reg);   /* 从指定寄存器读取 32 位值 */
}

/* ================================================================
 *  第6章: TD（传输描述符）分配与初始化
 *
 *  TD 是 UHCI 数据传输的基本单位。每个 TD 代表一次
 *  USB 总线事务。本驱动采用预分配池的策略：
 *  在初始化时一次性分配 MAX_TDS 个 TD，运行时从池中
 *  顺序分配（简化实现，无回收机制——每次控制传输后重置
 *  分配计数）。
 *
 *  uhci_alloc_td(): 从池中取出下一个空闲 TD，返回指针。
 *    如果池用尽（td_used >= MAX_TDS），返回 NULL。
 *    实际驱动应使用空闲链表管理，此处从简。
 *
 *  uhci_setup_td(): 初始化一个 TD 的四个字段。
 *    - link: 链指针，指向下一个 TD/QH 的物理地址
 *    - ctrl_status: 控制/状态字，函数内部会额外设置 ACTIVE 位
 *      通知硬件此 TD 待处理
 *    - token: 令牌字，包含 PID、地址、端点和长度
 *    - buffer: 数据缓冲区的物理（或虚拟，取决于映射）地址
 * ================================================================ */
/**
 * uhci_delay_ms - 忙等延时（毫秒级）
 * UHCI 的全局复位和端口复位要求至少 10ms 的 SE0 信号，
 * 设备枚举也需要若干毫秒的稳定时间。之前的实现用几十个
 * nop 代替（~100ns），设备根本没复位完成，第一个控制传输
 * 必然被 NAK 超时。用一个大循环做粗略的忙等延时。
 */
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
    td->link = link;                    /* 链接指针：指向链表中的下一个节点 */
    td->ctrl_status = ctrl_status | TD_CTRL_ACTIVE;  /* 设置控制位并标记为活跃，
                                                         让硬件开始处理 */
    td->token = token;                  /* 令牌包参数（PID + 地址 + 端点 + 长度） */
    td->buffer_ptr = buffer;            /* 数据缓冲区物理地址 */
}

/* ================================================================
 *  第7章: 等待 TD 完成（轮询方式）
 *
 *  当驱动程序将一个 TD 链挂入帧列表后，UHCI 硬件会在下一帧
 *  （帧列表到达该索引时）自动遍历并执行这些 TD。硬件执行
 *  TD 时会清零其 ACTIVE 位并设置状态位。
 *
 *  此函数通过轮询 TD 的 ctrl_status 字段来等待硬件处理完成。
 *  TD_CTRL_ACTIVE 被清除表示硬件已经处理完毕。
 *
 *  检查错误：若 ACTIVE 清零后任何错误标志（STALL、DBUF、
 *  BABBLE、NAK、CRCTIMEO、BITSTUFF、LENERR）被置位，
 *  函数返回 false 表示传输失败。
 *
 *  轮询与中断：
 *    本驱动使用轮询（busy waiting）而非中断来检测 TD 完成。
 *    这是因为：
 *    1. 轮询实现简单
 *    2. 在裸机/Hae 内核早期阶段，中断基础设施可能不完善
 *    3. 控制传输通常很快就完成（微秒级别）
 *    缺点：忙等浪费 CPU 周期。
 *
 *  timeout_us 参数当前未使用（void 转换抑制了编译器警告），
 *  实现使用硬编码的 100000 次迭代作为超时。
 * ================================================================ */
static bool uhci_wait_td(uhci_td_t *td, int timeout_us) {
    (void)timeout_us;
    /* ⚠️ 关键：帧列表里同一帧每 1024 帧才被处理一次（≈1.024s）。
     * TD 提交后要等当前帧号转一圈才轮到执行——等待必须覆盖
     * 一个完整周期（1.1s），否则必然超时。用 10ms 步进轮询。 */
    for (int i = 0; i < 110; i++) {          /* 110 × 10ms = 1.1s */
        if (!(td->ctrl_status & TD_CTRL_ACTIVE)) {
            /* ACTIVE 已清零，检查是否有错误发生 */
            if (td->ctrl_status & (TD_CTRL_STALL | TD_CTRL_DBUF | TD_CTRL_BABBLE |
                                   TD_CTRL_NAK | TD_CTRL_CRCTIMEO | TD_CTRL_BITSTUFF |
                                   TD_CTRL_LENERR))
                return false;   /* 检测到传输错误 */
            return true;        /* 传输成功完成 */
        }
        uhci_delay_ms(10);
    }
    return false;   /* 超时：1.1s 内 ACTIVE 未清零 */
}

/* ================================================================
 *  第7.5章: 同步批量传输（Bulk Transfer，BOT 用）
 *
 *  批量传输用于大块数据（U 盘读写）。BOT 协议的一次命令由
 *  三个批量事务组成：
 *    TD1: OUT 端点发送 CBW（31 字节，DATA0）
 *    TD2: IN/OUT 端点传输数据（DATA1 起，UHCI 硬件按 maxpacket 分包）
 *    TD3: IN 端点接收 CSW（13 字节）
 *
 *  与中断传输不同，批量传输没有固定轮询间隔，完成即止，
 *  因此采用"挂链→等待→恢复"的同步方式（同控制传输）。
 *  注意：qemu 的 max_len = (token>>21)+1，长度字段存 (长度-1)。
 * ================================================================ */
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

/* ================================================================
 *  第8章: 同步控制传输
 *
 *  控制传输（Control Transfer）是 USB 协议中最基础也是最
 *  重要的传输类型。它用于：
 *    - 设备枚举（获取描述符、设置地址、设置配置）
 *    - 设备配置（读取/设置设备属性）
 *    - 设备控制（如 HID 设备的 Set_Idle、Set_Protocol）
 *
 *  控制传输在 USB 协议层包含两个或三个阶段：
 *    1. 设置阶段（Setup Stage）：主机发送 8 字节的
 *       标准 USB 请求包到设备的默认端点（端点 0）。
 *       使用 SETUP 令牌包 + DATA0 数据包。
 *    2. 数据阶段（Data Stage）：可选。根据请求类型，
 *       数据可能从主机到设备（OUT）或设备到主机（IN）。
 *       使用 OUT/IN 令牌包，数据包序列为 DATA1、DATA0
 *       交替（以保持同步）。
 *    3. 状态阶段（Status Stage）：报告传输整体结果。
 *       方向与数据阶段相反。设备返回 ACK 表示成功，
 *       STALL 表示出错。
 *
 *  在 UHCI 层面，每个阶段映射为一个 TD：
 *    TD1（SETUP）→ TD2（DATA IN/OUT）→ TD3（STATUS）
 *  这些 TD 通过 link 字段串联成链表。
 *
 *  本函数执行过程：
 *    1. 构造 8 字节 setup 包（标准 USB 请求格式）
 *    2. 分配并设置 SETUP TD、DATA TD（如有数据）、STATUS TD
 *    3. 将 TD 链接成链：SETUP → DATA → STATUS
 *    4. 将链首 TD 的地址放入帧列表第 0 帧
 *    5. 等待 SETUP TD 完成（轮询）
 *    6. 清空帧列表第 0 帧，重置 TD 池以供下次使用
 *
 *  注意：本实现将所有控制传输放到帧 0 执行，这是简化方案。
 *  真正的驱动应根据当前帧号选择合适的帧位置，以避免在
 *  同一帧上堆积过多 TD 导致超时。
 * ================================================================ */
static int uhci_control_transfer(usb_hc_t *hc, uint8_t dev_addr,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 void *data, uint16_t wLength) {
    (void)hc;

    /* 记录进入时的 TD 使用量：控制传输只回收自己分配的 TD，
     * 不能把中断传输（键盘/鼠标）的 TD 也一起回收。 */
    int saved_td_used = uhci_priv.td_used;
    /* 调度 QH 的垂直链可能挂着中断传输（键盘/鼠标）的 TD——
     * 保存并在结束时恢复，避免控制传输打断中断轮询。 */
    uint32_t saved_el = schedule_qh->el_link;

    /* ---- Step 1: 构造 USB 标准请求的 8 字节 setup 包 ----
     *
     *  字节 0 (bmRequestType):
     *     位 7: 方向（0=主机到设备, 1=设备到主机）
     *     位 6-5: 类型（0=标准, 1=类, 2=厂商）
     *     位 4-0: 接收者（0=设备, 1=接口, 2=端点, 3=其他）
     *  字节 1 (bRequest): 请求号（如 GET_DESCRIPTOR=6,
     *     SET_ADDRESS=5, SET_CONFIGURATION=9）
     *  字节 2-3 (wValue): 请求特定参数（如描述符类型和索引）
     *  字节 4-5 (wIndex): 请求特定索引（如接口号或端点号）
     *  字节 6-7 (wLength): 后续数据阶段期望传输的字节数
     */
    uint8_t setup[8] = {
        bmRequestType, bRequest,
        (uint8_t)(wValue & 0xFF), (uint8_t)(wValue >> 8),
        (uint8_t)(wIndex & 0xFF), (uint8_t)(wIndex >> 8),
        (uint8_t)(wLength & 0xFF), (uint8_t)(wLength >> 8)
    };

    /* ---- Step 2: 分配 TD ----
     *  控制传输需要：
     *  - SETUP TD: 发送 8 字节 setup 包（必须）
     *  - DATA TD: 传输数据（仅当 wLength > 0 时）
     *  - STATUS TD: 状态阶段（必须）
     */
    uhci_td_t *td_setup = uhci_alloc_td();
    uhci_td_t *td_data = NULL;
    uhci_td_t *td_status = uhci_alloc_td();
    if (!td_setup || !td_status) return -1;

    /* 构造令牌字（token 字段）：
     *  位 0-7:   PID（包类型：SETUP/IN/OUT）
     *  位 8-14:  设备地址（dev_addr，7 位，范围 1-127）
     *  位 15:    端点号（此处固定为端点 0，即默认控制端点）
     *  位 16-26: 最大数据长度（11 位，单位字节）
     *
     *  注意：在本实现中，PID 放在位 0-7 的写法与标准 UHCI
     *  编程手册的位分配不完全一致。实际的 UHCI 硬件对 token
     *  字段的解析是由硬件逻辑决定的。这里的关键是硬件能
     *  正确识别出 PID、地址、端点和数据长度。
     */
    /* 注意：qemu 的 max_len = (token>>21)+1——长度字段存的是 (长度-1)！
     * 写 8 会被算成 9，SETUP 包大小 9≠8 → do_token_setup 直接 STALL。 */
    uint32_t token_setup = (dev_addr << 8) | (0 << 15) | (0 << 19) | (7 << 21) | (TD_TOKEN_PID_SETUP << 0);
    uint32_t token_in   = (dev_addr << 8) | (0 << 15) | (1 << 19) | ((wLength - 1) << 21) | (TD_TOKEN_PID_IN << 0);
    uint32_t token_out  = (dev_addr << 8) | (0 << 15) | (1 << 19) | ((wLength - 1) << 21) | (TD_TOKEN_PID_OUT << 0);

    /* 初始化 SETUP TD：发送 8 字节的 USB 请求数据 */
    uhci_setup_td(td_setup, 0, 0, token_setup, (uint32_t)(uintptr_t)setup);

    /* ---- Step 3: 链接 TD 链 ----
     *  如果 wLength > 0，则有数据阶段：SETUP → DATA → STATUS
     *  否则直接：SETUP → STATUS
     *
     *  链接指针的最低两位有特殊含义：
     *    位 0: 1 = 下一个节点是 TD，0 = 是 QH
     *    位 1: 垂直结束标志（Vertical Flag），1 表示链表结束
     *  此处 0x01 表示"下一个是 TD，且链表继续"。
     */
    if (wLength > 0) {
        td_data = uhci_alloc_td();
        if (!td_data) return -1;
        if (bmRequestType & 0x80) {
            /* 设备到主机（IN）：数据从设备读取到内存 */
            uhci_setup_td(td_data, 0, 0, token_in, (uint32_t)(uintptr_t)data);
        } else {
            /* 主机到设备（OUT）：数据从内存写入设备 */
            uhci_setup_td(td_data, 0, 0, token_out, (uint32_t)(uintptr_t)data);
        }
        /* 链接：SETUP TD → DATA TD（⚠️ 不能置 bit0=1：qemu 的 is_valid()
         * 要求链接位 bit0=0 才有效，bit0=1 被当成链表结束！
         * 0x01 只用于终止符。） */
        td_setup->link = (uint32_t)(uintptr_t)td_data;
        /* 链接：DATA TD → STATUS TD */
        td_data->link = (uint32_t)(uintptr_t)td_status;
    } else {
        /* 无数据阶段：SETUP → STATUS */
        td_setup->link = (uint32_t)(uintptr_t)td_status;
    }

    /* ---- Step 4: 构造 STATUS TD ----
     *  状态阶段的方向与数据阶段相反：
     *  如果数据阶段是 IN（设备→主机），则状态阶段是 OUT（主机→设备）
     *  如果数据阶段是 OUT（主机→设备），则状态阶段是 IN（设备→主机）
     *  如果无数据阶段，则状态阶段是 IN（设备→主机）
     *  STATUS TD 设置 IOC 位，使硬件在完成后产生中断。
     *  缓冲区地址为 0，因为状态阶段不传输数据（只传输握手包）。
     */
    uint32_t token_status;
    if (bmRequestType & 0x80)
        /* 数据阶段是 IN → 状态阶段用 OUT */
        token_status = (dev_addr << 8) | (0 << 15) | (1 << 19) | (0x7FF << 21) | (TD_TOKEN_PID_OUT << 0);
    else
        /* 数据阶段是 OUT → 状态阶段用 IN */
        token_status = (dev_addr << 8) | (0 << 15) | (1 << 19) | (0x7FF << 21) | (TD_TOKEN_PID_IN << 0);
    uhci_setup_td(td_status, 0x01, TD_CTRL_IOC, token_status, 0);  /* link=0x01 终止链 */

    /* ---- Step 5: 将 TD 链挂到调度 QH 的垂直链 ----
     *  qemu 每帧（1ms）处理一次帧列表，沿 QH 进入 TD 链。
     *  链接位 bit0 必须为 0（qemu 的 is_valid 判定），
     *  0x01 只用于终止符。 */
    schedule_qh->el_link = (uint32_t)(uintptr_t)td_setup;

    /* ---- Step 6: 等待传输完成 ----
     *  通过轮询 td_setup 的 ACTIVE 位判断是否完成。
     *  注意：等待 SETUP TD 完成通常意味着整个链都已完成，
     *  因为硬件会顺序处理。但严格来说应等待 STATUS TD。 */
    bool ok = uhci_wait_td(td_setup, 100000);

    /* ---- Step 7: 清理 ----
     *  恢复调度 QH 垂直链（中断 TD 若存在），TD 使用计数回滚到
     *  进入时的值——只释放本控制传输自己分配的 TD。 */
    schedule_qh->el_link = saved_el;
    uhci_priv.td_used = saved_td_used;
    if (!ok) {
        /* 调试：打印各 TD 状态 + 帧号（确认是否绕完一个帧周期） */
        vga_write("[USB-CTL] fail st="); vga_write_hex(td_setup->ctrl_status);
        if (td_data) { vga_write(" data="); vga_write_hex(td_data->ctrl_status); }
        vga_write(" status="); vga_write_hex(td_status->ctrl_status);
        vga_write(" frnum="); vga_write_hex(uhci_readw(UHCI_FRNUM));
        vga_write("\n");
    }
    return ok ? 0 : -1;
}

/* ================================================================
 *  第9章: 中断传输（轮询式实现）
 *
 *  中断传输（Interrupt Transfer）是 USB 四种传输类型之一。
 *  它用于需要定期轮询以获取数据的设备，如键盘、鼠标（HID）
 *  等输入设备。虽然名为"中断"传输，但在 USB 协议层面，
 *  它实际上是主机**定期轮询**设备的过程，而非设备主动发送中断。
 *
 *  本实现使用轮询（polling）方式检查中断传输是否完成。
 *  usb_poll() 函数必须被周期性地调用（如在主循环或定时器中）
 *  来检查 TD 是否已被硬件处理完毕。
 *
 *  数据结构：
 *    uhci_int_urb_t: 保存每个活跃中断传输的 URB（USB Request
 *    Block）和对应的 TD。最多支持 4 个并发中断传输。
 *
 *  uhci_interrupt_transfer():
 *    1. 从池中分配一个 TD，配置为 IN 传输
 *    2. 将 TD 挂入帧列表第 0 帧（简化实现）
 *    3. 分配并初始化一个 URB 结构体，记录回调函数
 *    4. 将 URB 加入 int_urbs 数组
 *
 *  注意：真正的驱动应将中断 TD 挂在合适的帧上以匹配
 *  设备的轮询间隔（bInterval），而非全部挂到帧 0。
 *  挂到帧 0 意味着每 1ms 查询一次设备，对于键盘（通常
 *  需要 10ms 间隔）来说过于频繁，浪费总线带宽。
 * ================================================================ */
typedef struct {
    usb_urb_t *urb;     /* USB 请求块，包含参数和回调函数 */
    uhci_td_t *td;      /* 对应的传输描述符 */
    bool active;        /* 该中断传输是否仍在活跃状态 */
    int dt;             /* ⚠️ 修复（留意项 1）：DATA toggle——每次重提交交替 */
} uhci_int_urb_t;

/* 中断传输 URB 表，最多跟踪 4 个活跃的中断传输 */
static uhci_int_urb_t int_urbs[4];
static int int_urb_count = 0;   /* 当前中断传输计数 */

static int uhci_interrupt_transfer(usb_hc_t *hc, uint8_t dev_addr,
                                   uint8_t ep, uint8_t *buffer, uint16_t len,
                                   void (*callback)(usb_urb_t *)) {
    (void)hc;

    /* ---- 优先复用：同端点已完成的中断传输（回调重提交场景） ----
     *  键盘/鼠标的回调在收到报告后会重新提交中断传输以维持轮询。
     *  如果每次都新分配 TD/URB，64 个 TD 的池很快耗尽。
     *  这里找到同 (dev_addr, ep) 的已完成传输，复用它的 TD 和 URB。 */
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

    /* ---- 首次提交：新分配 TD 和 URB ---- */
    /* 检查是否还有空位记录新的中断传输 */
    if (int_urb_count >= 4) return -1;

    /* 分配并初始化一个 TD，用于从设备读取数据 */
    uhci_td_t *td = uhci_alloc_td();
    if (!td) return -1;

    /* 构造令牌字：IN 传输，指定设备地址、端点和数据长度 */
    uint32_t token = (dev_addr << 8) | (ep << 15) | (0 << 19) | ((len - 1) << 21) | (TD_TOKEN_PID_IN << 0);
    /* ⚠️ 修复（留意项 2）：link=0x01 终止（首提同样适用） */
    uhci_setup_td(td, 0x01, TD_CTRL_IOC, token, (uint32_t)(uintptr_t)buffer);

    /* 挂到调度 QH 垂直链（bit0=0，qemu is_valid 要求） */
    schedule_qh->el_link = (uint32_t)(uintptr_t)td;

    /* 分配并初始化 URB 结构体，记录回调信息 */
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

    /* 将中断传输信息记录到 int_urbs 表 */
    int_urbs[int_urb_count].urb = urb;
    int_urbs[int_urb_count].td = td;
    int_urbs[int_urb_count].active = true;
    int_urbs[int_urb_count].dt = 0;   /* 首提 DATA0 */
    int_urb_count++;
    return 0;
}

/* ================================================================
 *  第10章: USB 中断传输轮询函数
 *
 *  此函数必须在主循环（或定时器中断）中被定期调用。
 *  它遍历 int_urbs 数组中所有活跃的中断传输，检查
 *  其 TD 的 ACTIVE 位是否已被硬件清除。
 *
 *  当检测到 TD 完成时：
 *    1. 标记该中断传输为非活跃
 *    2. 从 TD 的状态字中提取实际传输长度
 *    3. 调用 URB 中注册的回调函数通知上层
 *
 *  注意：本实现中，一次中断传输完成后不会自动重新提交。
 *  实际驱动应该重新准备 TD 并再次挂入帧列表，以实现
 *  持续的轮询（例如键盘驱动需要每帧都检测按键状态）。
 *  这里将 active 置为 false 后不再重新提交 TD，是简化实现。
 * ================================================================ */
void usb_poll(void) {
    for (int i = 0; i < int_urb_count; i++) {
        if (!int_urbs[i].active) continue;  /* 跳过已完成或取消的中断传输 */
        uhci_td_t *td = int_urbs[i].td;
        if (!(td->ctrl_status & TD_CTRL_ACTIVE)) {
            /* 硬件已处理完此 TD，传输完成 */
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
                /* 从 TD 控制状态字提取实际传输的长度（位 16-26） */
                urb->length = (td->ctrl_status >> 16) & 0x7FF;
                urb->callback(urb);   /* 调用上层注册的回调函数 */
            }
        }
    }
}

/* ================================================================
 *  第11章: URB 调度器（根据类型分发）
 *
 *  URB（USB Request Block）是 USB 子系统中的核心抽象。
 *  它封装了一次 USB 传输请求的所有参数：
 *    - 传输类型（控制、中断、批量、等时）
 *    - 目标设备地址和端点
 *    - 数据缓冲区指针和长度
 *    - 回调函数（异步完成时调用）
 *
 *  此函数是 UHCI 驱动向 USB 核心层提供的接口。USB 核心
 *  层通过 hc->submit_urb 调用此函数，驱动根据 URB 的
 *  传输类型将请求分发给相应的处理函数：
 *    - USB_URB_CONTROL → uhci_control_transfer()
 *    - USB_URB_INTERRUPT → uhci_interrupt_transfer()
 *    - 其他类型 → 尚未实现，返回 -1
 *
 *  这种分发模式（dispatch pattern）是一种典型的软件架构
 *  分层：USB 核心层定义通用接口，主机控制器驱动提供具体实现。
 * ================================================================ */
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

/* ================================================================
 *  第12章: USB 设备枚举
 *
 *  设备枚举（Enumeration）是 USB 协议中最关键的过程。
 *  它在新设备插入时由主机驱动程序执行，目的是识别设备
 *  并为它分配一个唯一的 USB 地址。枚举过程如下：
 *
 *  Step 1: 获取设备描述符前 8 字节（Get Device Descriptor）
 *    使用默认地址 0（所有未分配地址的设备默认地址为 0），
 *    请求读取设备描述符的头 8 字节。这 8 字节包含了
 *    关键信息：bMaxPacketSize0（端点 0 的最大包大小），
 *    后续通信需要这个值。
 *
 *  Step 2: 设置地址（Set Address）
 *    分配一个新的 USB 地址（1~127）给设备。此后，所有
 *    与该设备的通信都必须使用这个新地址。
 *
 *  Step 3: 获取完整设备描述符（Get Full Device Descriptor）
 *    使用新地址读取完整的设备描述符（18 字节），包含
 *    VID、PID、USB 版本号、设备类、子类、协议等信息。
 *
 *  Step 4: 获取配置描述符（Get Configuration Descriptor）
 *    读取配置描述符以了解设备有多少个接口、每个接口的
 *    端点信息。配置描述符是变长结构：先读 9 字节头部
 *    获取总长度 wTotalLength，再读取完整数据。
 *
 *  Step 5: 设置配置（Set Configuration）
 *    选中一个配置值（bConfigurationValue）并通知设备。
 *    设备被配置后才能正常运行。
 *
 *  Step 6: 注册设备
 *    将设备信息（地址、描述符等）加入系统设备链表，
 *    供上层驱动（如 HID 驱动、Mass Storage 驱动等）使用。
 *
 *  错误处理：每个步骤都可能失败（设备断开、STALL 等），
 *  枚举函数在任一步骤失败时及时返回并打印错误信息。
 * ================================================================ */

/*
 *  get_descriptor: 从 USB 设备读取指定描述符
 *  通过 usb_control_transfer 发送 GET_DESCRIPTOR 标准请求。
 *  参数：
 *    hc:         USB 主机控制器指针
 *    dev_addr:   USB 设备地址
 *    desc_type:  描述符类型（DEVICE=1, CONFIGURATION=2 等）
 *    desc_index: 描述符索引（对于多配置设备）
 *    buf:        数据接收缓冲区
 *    len:        请求读取的字节数
 */
static int get_descriptor(usb_hc_t *hc, uint8_t dev_addr,
                          uint8_t desc_type, uint8_t desc_index,
                          void *buf, uint16_t len) {
    return usb_control_transfer(hc, dev_addr,
                                0x80, USB_REQ_GET_DESCRIPTOR,
                                (desc_type << 8) | desc_index,
                                0, buf, len);
}

/*
 *  set_address: 为 USB 设备分配新地址
 *  发送 SET_ADDRESS 标准请求（bmRequestType=0x00，
 *  接收者=设备，类型=标准，方向=主机到设备）。
 *  wValue 包含要分配的新地址（1-127）。
 *  注意：此请求发送时设备仍使用旧地址（通常是 0），
 *  设备在收到请求并回复 ACK 后才切换到新地址。
 */
static int set_address(usb_hc_t *hc, uint8_t old_addr, uint8_t new_addr) {
    return usb_control_transfer(hc, old_addr,
                                0x00, USB_REQ_SET_ADDRESS,
                                new_addr, 0, NULL, 0);
}

/*
 *  enumerate_device: 枚举指定端口上的 USB 设备
 *
 *  这是 USB 子系统中最核心的函数之一。它实现了完整的
 *  设备枚举流程，识别新插入的设备并将其注册到系统。
 *
 *  参数：
 *    hc:    USB 主机控制器指针
 *    port:  端口号（0 或 1）
 *
 *  枚举成功后，设备信息会被添加到全局设备列表
 *  （通过 usb_register_device），上层驱动可以遍历此列表
 *  查找它们支持的设备。
 *
 *  本实现不支持多配置设备的选择（Config > 1），仅选择
 *  设备的第一个有效配置值。
 */
static void enumerate_device(usb_hc_t *hc, uint8_t port) {
    (void)port;    /* 当前实现未使用 port 参数 */

    /* ---- Step 1: 读 8 字节设备描述符 ----
     *  使用默认地址 0，仅读前 8 字节以获取 bMaxPacketSize0
     *  （设备端点 0 的最大包大小，位于描述符的第 8 个字节）。
     *  这是后续所有通信的基础。 */
    uint8_t buf[8];
    int ret = get_descriptor(hc, 0, USB_DESC_DEVICE, 0, buf, 8);
    if (ret < 0) {
        vga_write("[USB] Get device desc (8) failed.\n");
        return;
    }
    uint8_t max_packet = buf[7];    /* bMaxPacketSize0 偏移量 = 7 */
    (void)max_packet;    /* 本实现尚未使用此值进行后续通信 */

    /* ---- Step 2: 分配地址并设置 ----
     *  分配一个唯一地址（从 1 开始递增），发送 SET_ADDRESS
     *  请求。设备完成地址切换后，后续通信使用新地址。 */
    uint8_t new_addr = uhci_priv.next_address++;
    if (new_addr == 0) new_addr = 1;  /* 地址 0 保留给默认状态 */
    ret = set_address(hc, 0, new_addr);
    if (ret < 0) {
        vga_write("[USB] Set address failed.\n");
        return;
    }

    /* 设备需要一小段时间来稳定状态（复位恢复时间），规范建议至少 2ms */
    uhci_delay_ms(2);

    /* ---- Step 3: 读完整设备描述符 ----
     *  使用新地址获取完整的 18 字节设备描述符。
     *  关键字段：
     *    idVendor:   USB 供应商 ID（如 0x046D = Logitech）
     *    idProduct:  产品 ID
     *    bcdUSB:     支持的 USB 规范版本（如 0x0110 = USB 1.1）
     *    bDeviceClass: 设备类（0 = 每个接口自行定义） */
    usb_device_descriptor_t dev_desc;
    ret = get_descriptor(hc, new_addr, USB_DESC_DEVICE, 0, &dev_desc, sizeof(dev_desc));
    if (ret < 0) {
        vga_write("[USB] Get full device desc failed.\n");
        return;
    }

    /* ---- Step 4: 读配置描述符 ----
     *  先读 9 字节头部获取总长度 wTotalLength，
     *  再读取完整配置描述符数据。
     *  配置描述符是复合结构：头部 + 一个或多个接口描述符
     *  （Interface Descriptor）+ 若干个端点描述符
     *  （Endpoint Descriptor）的集合。
     *
     *  配置描述符的主要字段：
     *    wTotalLength:      整个配置数据的长度（含接口和端点）
     *    bNumInterfaces:    配置包含的接口数
     *    bConfigurationValue: 选择此配置时要使用的值
     *    bmAttributes:      配置特性（自供电、远程唤醒等）
     *    bMaxPower:         最大功耗（单位 2mA） */
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

    /* ---- Step 5: 将设备注册到系统 ----
     *  分配设备结构体，填充设备信息（地址、描述符），
     *  然后加入到全局设备链表中。上层 USB 类驱动
     *  （如 HID、Mass Storage）将通过 usb_get_device_list()
     *  遍历此链表来发现新设备。 */
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

    /* ---- Step 6: 设置配置 ----
     *  使设备进入配置状态（Configured State）。只有进入
     *  此状态后，非端点 0 的传输才能进行。 */
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

/* ============================================================
 *  USB HUB 支持（class 0x09）
 * ============================================================ */

/**
 * usb_hub_port_count - 读取 hub 的端口数（HUB 描述符第 2 字节 bNbrPorts）
 */
static uint8_t usb_hub_port_count(usb_hc_t *hc, uint8_t hub_addr) {
    uint8_t buf[8];
    int ret = usb_control_transfer(hc, hub_addr,
                                   0x80, USB_REQ_GET_DESCRIPTOR,
                                   (0x29 << 8) | 0,   /* HUB 描述符 */
                                   0, buf, sizeof(buf));
    if (ret < 0) return 0;
    return buf[2];   /* bNbrPorts */
}

/**
 * usb_hub_scan - 扫描 hub 的下游端口，枚举每个已连接设备（递归支持级联）
 *
 * 流程（每个端口）：
 *   GET_PORT_STATUS → CCS 位判断有无设备
 *   → SET_FEATURE(PORT_POWER) 供电 → SET_FEATURE(PORT_RESET) 复位
 *   → 等待复位完成 → enumerate_device（下游设备从地址 0 开始，
 *     hub 会把地址 0 的控制请求转发到刚复位的端口）
 *   → 若下游设备是 hub（class 0x09），enumerate_device 内部递归
 */
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

/* ================================================================
 *  第13章: UHCI 控制器初始化
 *
 *  这是 UHCI 驱动最关键的初始化函数。它按严格的顺序执行
 *  一系列操作来唤醒并配置 UHCI 硬件，使 USB 总线开始运行。
 *
 *  初始化流程：
 *    1. 保存 I/O 基地址
 *    2. 全局复位（GRESET）：向 USB 总线发送复位信号，
 *       所有设备被重置到默认地址 0。至少保持 10ms。
 *    3. 分配帧列表（Frame List）：4KB 物理连续内存，
 *       初始化为空指针（0x01 表示 QH 终止）。
 *    4. 分配 TD 池：预分配 MAX_TDS 个 TD（物理连续内存）。
 *    5. 设置 FRBASE 寄存器：告诉硬件帧列表的物理地址。
 *    6. 启用中断：设置 INTR 寄存器的短包中断位。
 *    7. 启动控制器：设置 CMD 寄存器的 RS（运行）和
 *       CF（配置完成）位。至此，UHCI 开始在 USB 总线上
 *      发送 SOF 包。
 *    8. 扫描端口：检查每个端口是否有设备连接。如果有，
 *       启用该端口（设置 PE 位）并为连接的设备执行
 *       枚举流程。
 *
 *  为什么必须先分配帧列表和 TD 池再启动？
 *    因为 UHCI 一旦启动（RS=1），硬件会立即开始遍历
 *    帧列表。如果帧列表指针是空或无效，会导致硬件异常。
 *    所以必须在启动前设置好所有内存结构。
 * ================================================================ */
static void uhci_init(usb_hc_t *hc) {
    /* ---- 第一步: 保存 I/O 基地址 ----
     *  BAR0 包含 UHCI 寄存器的 I/O 基地址。所有后续的
     *  寄存器读写都以此为基准偏移。 */
    uhci_priv.io_base = hc->base_addr;
    vga_write("[UHCI] Init at I/O 0x");
    vga_write_hex(uhci_priv.io_base);
    vga_write("\n");

    /* ---- 第二步: 全局复位（Global Reset） ----
     *  向 CMD 寄存器写入 GRESET 位：
     *    - 在 USB 总线上发送至少 10ms 的复位信号 SE0
     *    - 所有已连接的设备被复位到默认地址 0
     *    - 设备处于 Default 状态，等待主机分配地址
     *  硬件自动在 10ms 后完成复位并清零 GRESET 位。 */
    uhci_writew(UHCI_CMD, UHCI_CMD_GRESET);
    uhci_delay_ms(10);   /* 全局复位需保持 ≥10ms */
    uhci_writew(UHCI_CMD, 0);
    uhci_delay_ms(1);

    /* ---- 第三步: 分配帧列表 ----
     *  帧列表是 UHCI 最核心的数据结构，位于主机内存中，
     *  由 UHCI 硬件通过 DMA 读取。
     *
     *  FRAME_LIST_SIZE = 1024 项
     *  每项 4 字节（32 位物理地址指针）
     *  总计 4KB（1 页物理内存）
     *
     *  初始值：所有 1024 项都设为 0x01。0x01 的含义：
     *    位 0 = 1: 表示指向的是 QH（队列头）
     *    位 1 = 0: 表示这是队列的结束（Vertical Flag=0
     *              意味着"继续"实际上此处应设为终止）
     *  实际上 0x01 表示一个空的终止队列——当 UHCI 遇到
     *  地址为 0x01 的 QH/TD 时，认为链表已结束，不再处理
     *  该帧。 */
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

    /* ---- 第四步: 分配 TD 池 ----
     *  预分配 MAX_TDS 个 TD 的连续物理内存。
     *  每个 TD 16 字节（严格 16 字节对齐）。
     *  总大小 = MAX_TDS * sizeof(uhci_td_t)
     *  向上取整到页边界。
     *
     *  为什么预分配而非动态分配？
     *    1. TD 需要物理连续内存（DMA 要求）
     *    2. 预分配避免运行时分配失败
     *    3. 控制传输需要快速分配 TD */
    int td_pages = (MAX_TDS * sizeof(uhci_td_t) + 4095) / 4096;
    uhci_priv.td_pool = (uhci_td_t*)alloc_phys_pages(td_pages, &uhci_priv.td_pool_phys);
    if (!uhci_priv.td_pool) {
        vga_write("[UHCI] TD pool alloc failed.\n");
        return;
    }
    uhci_priv.td_used = 0;
    uhci_priv.next_address = 1;  /* USB 设备地址从 1 开始分配 */

    /* ---- 第五步: 设置帧列表基址寄存器 ---- */
    uhci_writel(UHCI_FRBASE, uhci_priv.frame_list_phys);

    /* ---- 第六步: 设置中断使能 ----
     *  设置 INTR 寄存器位 0（USBINT 中断使能）。
     *  启用后，带有 IOC 位的 TD 完成时会触发中断。
     *  注意：由于 CF 位尚未设置，部分中断可能被屏蔽。 */
    uhci_writew(UHCI_INTR, 0x0001); /* 使能短包/传输完成中断 */

    /* ---- 第七步: 启动 UHCI 控制器 ----
     *  同时设置 RS 和 CF 位：
     *    RS (Run/Stop) = 1: 启动帧列表处理。UHCI 开始
     *      以 1ms 为周期遍历帧列表，在 USB 总线上发送
     *      SOF 包并执行帧列表中的 TD 链。
     *    CF (Configured Flag) = 1: 标记驱动配置完成。
     *      只有 CF=1 时，端口状态变化才会产生中断。
     *      软件应确保在设置 CF 前完成所有配置。 */
    uhci_writew(UHCI_CMD, UHCI_CMD_RS | UHCI_CMD_CF);
    uhci_priv.running = true;


    /* ---- 第八步: 扫描 USB 端口 ----
     *  UHCI 控制器有两个端口（PORT1 和 PORT2）。
     *  检查每个端口的 CCS（Current Connect Status）位
     *  判断是否有设备连接。
     *
     *  对于每个有设备连接的端口：
     *    1. 端口复位（设置 RD 位）：强制设备复位
     *    2. 使能端口（设置 PE 位）：允许端口进行传输
     *    3. 调用 enumerate_device() 进行设备枚举 */
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


            /* 枚举连接的设备 */
            enumerate_device(hc, port);
        }
    }
}

/* ================================================================
 *  第14章: 全局 USB 子系统管理
 *
 *  此部分代码构成了 USB 子系统的基础框架层。它维护了：
 *    1. 主机控制器列表（hc_list）：系统中所有 USB 主机
 *       控制器（UHCI）的数组。最多支持 8 个控制器。
 *    2. 设备链表（device_list）：所有已枚举的 USB 设备
 *       的单向链表。
 *
 *  这是操作系统内核 USB 子系统中最顶层的管理结构。
 *  架构上分为三层：
 *
 *    ┌─────────────────────────────────────────┐
 *    │   USB 类驱动（HID、Mass Storage 等）     │
 *    │   通过 usb_get_device_list() 发现设备     │
 *    │   通过 usb_submit_urb() 发起传输         │
 *    └─────────────┬───────────────────────────┘
 *    ┌─────────────▼───────────────────────────┐
 *    │   USB 核心层                              │
 *    │   usb_control_transfer()                 │
 *    │   usb_submit_urb()                        │
 *    │   usb_register_device()                   │
 *    │   构造 URB → 调用 hc->submit_urb()        │
 *    └─────────────┬───────────────────────────┘
 *    ┌─────────────▼───────────────────────────┐
 *    │   主机控制器驱动（UHCI/OHCI/EHCI/xHCI）  │
 *    │   → 本文件：uhci_submit_urb()             │
 *    │   → 构造 TD 链 → 挂入帧列表              │
 *    │   → UHCI 硬件 DMA 执行 TD                │
 *    └─────────────────────────────────────────┘
 *
 *  usb_control_transfer():
 *    这是 USB 核心层提供的便捷函数。它构造一个 URB，设置
 *    传输类型为 CONTROL，填充请求参数，然后调用主机控制器
 *    驱动的 submit_urb 接口来实际执行传输。
 *
 *  usb_submit_urb():
 *    USB 核心层提交 URB 的通用接口。它检查 hc 和 submit_urb
 *    的有效性后，调用底层驱动实现。
 * ================================================================ */

/* 主机控制器列表：系统中可被发现的 UHCI 控制器数组。
 * 通过 PCI 总线扫描填充。*/
static usb_hc_t hc_list[8];
static int hc_count = 0;       /* 已发现的主机控制器数量 */

/* USB 设备链表头：所有已成功枚举的 USB 设备构成的链表。
 * 通过 usb_register_device() 添加。*/
static usb_device_t *device_list = NULL;

/*
 *  usb_register_device: 将枚举成功的设备添加到全局设备链表
 *  新设备插入链表头部（头插法，实现简单）。
 *  上层驱动通过 usb_get_device_list() 遍历此链表。
 */
void usb_register_device(usb_device_t *dev) {
    dev->next = device_list;
    device_list = dev;
}

/*
 *  usb_get_device_list: 返回全局设备链表头
 *  供 USB 类驱动（如 HID、Mass Storage 驱动）遍历查找设备。
 */
usb_device_t* usb_get_device_list(void) {
    return device_list;
}

/*
 *  usb_register_hc: 注册一个发现的主机控制器
 *  由 PCI 扫描函数调用，将找到的 UHCI 控制器加入 hc_list。
 */
void usb_register_hc(usb_hc_t *hc) {
    if (hc_count < 8) hc_list[hc_count++] = *hc;
}

/*
 *  usb_control_transfer: USB 核心层提供的控制传输封装函数
 *  调用者提供完整的 USB 请求参数，函数内部构造 URB 然后
 *  调用主机控制器驱动的 submit_urb 接口。
 *
 *  参数说明（与 USB 规范完全一致）：
 *    bmRequestType: 请求类型字节
 *    bRequest:      具体请求号
 *    wValue:        请求参数（16 位）
 *    wIndex:        索引（16 位，接口/端点号）
 *    data:          数据缓冲区
 *    wLength:       数据长度
 */
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

/*
 *  usb_submit_urb: 提交通用 URB 的接口
 *  供上层驱动对非控制传输（如中断传输、批量传输）使用。
 *  直接传递已经构造好的 URB 结构体到底层驱动。
 */
int usb_submit_urb(usb_hc_t *hc, usb_urb_t *urb) {
    if (!hc || !hc->submit_urb) return -1;
    return hc->submit_urb(hc, urb);
}

/* ================================================================
 *  第15章: PCI 总线扫描 — 发现 UHCI 控制器
 *
 *  PCI（Peripheral Component Interconnect）总线是 x86 平台
 *  发现和配置外设的标准机制。UHCI 控制器作为 PCI 设备存在，
 *  需要通过 PCI 配置空间来定位它。
 *
 *  UHCI 在 PCI 配置空间中的标识：
 *    类别码（Class Code）= 0x0C（串行总线控制器）
 *    子类别码（Subclass） = 0x03（USB 控制器）
 *    编程接口（Prog IF） = 0x00（表示 UHCI 而非 OHCI
 *                          或 EHCI 等其他 USB 规范）
 *
 *  扫描过程：
 *    遍历所有总线（0-255）、设备（0-31）、功能（0-7），
 *    读取每个设备的 Vendor ID（供应商 ID）。0xFFFF 表示
 *    该位置不存在设备。对于存在的设备，检查其 Class Code
 *    和 Subclass 是否匹配 USB 控制器。
 *
 *  对于每个找到的 UHCI 控制器：
 *    1. 读取 BAR0 寄存器（偏移 0x10）：IO 基地址
 *    2. 构造 usb_hc_t 结构体
 *    3. 注册到 USB 子系统
 *
 *  注意：PCI 配置空间通过 I/O 端口 0xCF8（配置地址端口）
 *  和 0xCFC（配置数据端口）访问。pci_read_config 函数
 *  封装了这一访问过程。
 * ================================================================ */
static void probe_controllers(void) {
    /* 遍历所有 PCI 总线、设备和功能号 */
    for (int bus=0; bus<256; bus++) {
        for (int slot=0; slot<32; slot++) {
            for (int func=0; func<8; func++) {
                /* 读取 Vendor ID。0xFFFF 表示空槽。
                 * 对 function 0 返回 0xFFFF 的 slot，
                 * 表示整个设备不存在，跳出 function 循环。
                 * 对 function > 0 返回 0xFFFF，仅跳过当前。 */
                uint32_t vid = pci_read_config(bus, slot, func, 0) & 0xFFFF;
                if (vid == 0xFFFF) {
                    if (func == 0) break;
                    else continue;
                }

                /* 读取 Class Code 和 Subclass（在配置空间偏移 0x08 处）：
                 *   位 31-24: Class Code
                 *   位 23-16: Subclass
                 *   位 15-8:  Prog IF（编程接口）
                 *   位 7-0:   Revision ID */
                uint32_t class_rev = pci_read_config(bus, slot, func, 0x08);
                uint8_t class = (class_rev >> 24) & 0xFF;
                uint8_t subclass = (class_rev >> 16) & 0xFF;

                /* 检查是否为 USB 主机控制器 */
                if (class != 0x0C || subclass != 0x03) continue;

                /* 读取 I/O 基址：扫描全部 6 个 BAR（偏移 0x10~0x24），
                 * 取第一个非零值。不同设备的 I/O 基址放在不同 BAR：
                 *  - 多数 UHCI 用 BAR0（偏移 0x10）
                 *  - qemu 的 piix3-usb-uhci 用 BAR4（0x20——qemu 的 0 基
                 *    编号，配置空间实际偏移 0x20）！
                 * 只读固定偏移会拿到 0 而误判为无设备。 */
                uint32_t bar = 0;
                for (int b = 0; b < 6; b++) {
                    uint32_t v = pci_read_config(bus, slot, func, 0x10 + b * 4) & ~0x0F;
                    if (v) { bar = v; break; }
                }
                if (!bar) continue;   /* 没有有效的 I/O BAR */

                /* 构造并注册主机控制器 */
                usb_hc_t hc = {0};
                hc.bus = bus; hc.slot = slot; hc.func = func;
                hc.base_addr = bar;
                hc.type = USB_HC_UHCI;
                hc.init = uhci_init;           /* 初始化函数指针 */
                hc.submit_urb = uhci_submit_urb; /* URB 提交函数指针 */
                usb_register_hc(&hc);
                vga_write("[USB] UHCI found at ");
                vga_write_hex(bar);
                vga_write("\n");
            }
        }
    }
}

/* ================================================================
 *  第16章: USB 子系统全局初始化入口
 *
 *  usb_init() 是 USB 子系统的总入口，从操作系统内核的
 *  初始化流程中调用（通常在 PCI 枚举之后、设备驱动加载之前）。
 *
 *  执行流程：
 *    1. probe_controllers(): 扫描 PCI 总线发现所有 UHCI
 *       控制器并在 hc_list 中注册
 *    2. 如果没有任何控制器，返回
 *    3. 对每个已注册的控制调用 hc->init() 进行初始化，
 *       这个调用会进入 uhci_init() 完成 UHCI 的完整初始化
 *       流程（包括设备枚举）
 *
 *  初始化之后 USB 子系统完全就绪：
 *    - UHCI 硬件在运行，帧列表在处理
 *    - 已连接的设备已被枚举并注册
 *    - 上层驱动可以通过 usb_get_device_list() 发现设备
 *    - 可以通过 usb_control_transfer() 和 usb_submit_urb()
 *      与设备通信
 * ================================================================ */
void usb_init(void) {
    vga_write("[USB] Initializing...\n");

    /* Step 1: 发现所有 USB 主机控制器 */
    probe_controllers();
    if (hc_count == 0) {
        vga_write("[USB] No UHCI controller.\n");
        return;
    }

    /* Step 2: 初始化每个找到的控制器 */
    for (int i=0; i<hc_count; i++) {
        if (hc_list[i].init)
            hc_list[i].init(&hc_list[i]);
    }

    /* Step 3: 枚举设备已由 uhci_init 完成，
     * 现在让各子模块认领设备 */
    usb_hid_init();
    usb_mass_storage_init();

    vga_write("[USB] Init done.\n");
}

/* ================================================================
 *  第17章: HID 接口查找函数
 *
 *  这是 HID（Human Interface Device，人机接口设备）驱动
 *  使用的辅助函数。它遍历配置描述符数据，查找类型为
 *  HID（bInterfaceClass = 0x03）的接口，并返回该接口的
 *  描述符和相关的输入/输出端点描述符。
 *
 *  配置描述符的布局结构（解析关键）：
 *    ┌──────────────┐
 *    │ 配置描述符     │ ← 每个配置一个
 *    ├──────────────┤
 *    │ 接口描述符     │ ← 每个配置有 bNumInterfaces 个
 *    ├──────────────┤
 *    │ HID 描述符    │ ← HID 类特有的描述符
 *    ├──────────────┤
 *    │ 端点描述符 IN  │ ← 输入端点（设备→主机）
 *    ├──────────────┤
 *    │ 端点描述符 OUT │ ← 输出端点（主机→设备）
 *    ├──────────────┤
 *    │ ...（更多接口）│
 *    └──────────────┘
 *
 *  每个描述符头部的前两个字节通用：
 *    字节 0: bLength（描述符长度）
 *    字节 1: bDescriptorType（描述符类型代码）
 *  通过类型代码可以区分是哪种描述符，从而正确解析。
 *
 *  函数参数：
 *    config_data: 完整的配置描述符数据缓冲区
 *    total_len:   总长度
 *    out_iface:   输出参数，找到的接口描述符指针
 *    out_ep_in:   输出参数，第一个 IN 端点描述符
 *    out_ep_out:  输出参数，第一个 OUT 端点描述符
 *
 *  返回值：找到 HID 接口返回 true，否则 false。
 * ================================================================ */
bool find_hid_interface(usb_hc_t *hc, uint8_t dev_addr,
                        const uint8_t *config_data, uint16_t total_len,
                        usb_interface_descriptor_t **out_iface,
                        usb_endpoint_descriptor_t **out_ep_in,
                        usb_endpoint_descriptor_t **out_ep_out) {
    (void)hc; (void)dev_addr;    /* 参数暂未使用，保留接口兼容性 */

    uint16_t pos = 0;    /* 当前解析位置 */
    while (pos < total_len) {
        uint8_t len = config_data[pos];      /* 当前描述符长度 */
        if (len == 0) break;                 /* 零长度描述符 → 数据损坏，停止解析 */
        uint8_t type = config_data[pos+1];   /* 描述符类型 */

        if (type == USB_DESC_INTERFACE) {
            /* 遇到接口描述符，检查是否为 HID 类 */
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t*)&config_data[pos];
            if (iface->bInterfaceClass == 0x03) {   /* 0x03 = HID 设备类 */
                *out_iface = iface;
                pos += len;

                /* 在找到的接口描述符之后遍历其子描述符（端点描述符等），
                 * 收集输入和输出端点信息。*/
                while (pos < total_len) {
                    uint8_t l = config_data[pos];
                    if (l == 0) break;
                    uint8_t t = config_data[pos+1];
                    if (t == USB_DESC_ENDPOINT) {
                        /* 端点描述符，根据方向分类：
                         *  位 7 = 1: IN 端点（设备→主机）
                         *  位 7 = 0: OUT 端点（主机→设备）*/
                        usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t*)&config_data[pos];
                        if ((ep->bEndpointAddress & 0x80) && !*out_ep_in)
                            *out_ep_in = ep;     /* 记录第一个 IN 端点 */
                        else if (!(ep->bEndpointAddress & 0x80) && !*out_ep_out)
                            *out_ep_out = ep;    /* 记录第一个 OUT 端点 */
                    } else if (t == USB_DESC_INTERFACE) {
                        break;  /* 遇到下一个接口描述符，结束循环 */
                    }
                    pos += l;
                }
                return true;    /* 完成，返回找到的 HID 接口 */
            }
        }
        pos += len;    /* 移动到下一个描述符 */
    }
    return false;   /* 未找到 HID 接口 */
}
