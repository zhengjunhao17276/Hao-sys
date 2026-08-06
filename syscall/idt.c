/**
 * =========================================================================
 * idt.c - 中断描述符表（IDT）初始化
 *
 * 中断向量分配方案：
 *   0x00-0x13: CPU 异常（Exception 0~19），DPL=0（仅内核触发）
 *   0x14-0x1F: 保留（指向默认 isr_stub）
 *   0x20-0x2F: 硬件 IRQ（由 PIC 重映射至此），DPL=0
 *   0x30-0x7F: 保留（指向默认 isr_stub）
 *   0x80:      系统调用（通过 int 0x80），DPL=3（用户态可触发）
 *   0x81-0xFF: 保留（指向默认 isr_stub）
 *
 * 处理程序都在 isr_asm.asm 中实现：
 *   - exc0_handler ~ exc19_handler: 异常处理，显示异常号后死循环
 *   - irq0_handler ~ irq15_handler: IRQ 处理，转发给 C 处理函数
 *   - isr80_handler: 系统调用处理，转到 syscall_dispatcher()
 *   - isr_stub: 未知中断处理，输出 "EX:??" 后 EOI 并返回
 * =========================================================================
 */

#include "../include/syscall/idt.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"

/* 声明外部汇编处理程序入口点 */
extern void isr_stub(void);       /* 默认处理程序（未用的中断） */
extern void irq0_handler(void);   /* IRQ 处理程序 */
extern void irq1_handler(void);
extern void irq2_handler(void);
extern void irq3_handler(void);
extern void irq4_handler(void);
extern void irq5_handler(void);
extern void irq6_handler(void);
extern void irq7_handler(void);
extern void irq8_handler(void);
extern void irq9_handler(void);
extern void irq10_handler(void);
extern void irq11_handler(void);
extern void irq12_handler(void);
extern void irq13_handler(void);
extern void irq14_handler(void);
extern void irq15_handler(void);
extern void isr80_handler(void);  /* 系统调用处理程序（int 0x80） */

/* IDT（256 个条目 × 8 字节 = 2048 字节） */
static idt_entry_t idt[256];
static idtr_t idtr;               /* IDTR 加载结构 */

/**
 * idt_set_gate - 设置 IDT 的一个门
 * @num:     中断向量号（0~255）
 * @handler: 处理程序函数的 32 位线性地址
 * @selector: GDT 中的代码段选择子（内核用 0x08）
 * @flags:    属性字节：bit 7=P, bit 6-5=DPL, bit 4=0, bit 3-0=type
 *            - 0x8E: 中断门，P=1, DPL=0, type=0xE（32 位中断门）
 *            - 0xEE: 中断门，P=1, DPL=3, type=0xE（用户态可触发）
 */
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags) {
    idt[num].base_low  = handler & 0xFFFF;           /* 偏移低 16 位 */
    idt[num].base_high = (handler >> 16) & 0xFFFF;   /* 偏移高 16 位 */
    idt[num].selector  = selector;                    /* 代码段选择子 */
    idt[num].zero      = 0;                           /* 保留位必须为 0 */
    idt[num].flags     = flags;
}

/**
 * idt_init - 初始化完整 IDT
 *
 * 设置所有 256 个中断向量：
 *   1. 异常 0-19：使用专门的异常处理程序（exc*_handler），DPL=0
 *   2. 20-31：默认 isr_stub
 *   3. IRQ 32-47（0x20-0x2F）：对应 irq*_handler，DPL=0
 *   4. 系统调用 0x80：isr80_handler，DPL=3（允许用户态触发）
 *   5. 其他：默认 isr_stub
 *
 * 最后加载 IDTR 寄存器使 IDT 生效。
 */
void idt_init(void) {
    /* ---- 1. 设置 CPU 异常 0-19 ---- */
    /* 声明异常处理程序的外部符号 */
    extern void exc0_handler(void);
    extern void exc1_handler(void);
    extern void exc2_handler(void);
    extern void exc3_handler(void);
    extern void exc4_handler(void);
    extern void exc5_handler(void);
    extern void exc6_handler(void);
    extern void exc7_handler(void);
    extern void exc8_handler(void);
    extern void exc9_handler(void);
    extern void exc10_handler(void);
    extern void exc11_handler(void);
    extern void exc12_handler(void);
    extern void exc13_handler(void);
    extern void exc14_handler(void);
    extern void exc15_handler(void);
    extern void exc16_handler(void);
    extern void exc17_handler(void);
    extern void exc18_handler(void);
    extern void exc19_handler(void);

    /* 用数组指针的方式批量设置异常门——flags=0x8E 是中断门
     * （P=1, DPL=0, 32 位中断门），仅内核态可触发 */
    for (int i = 0; i < 20; i++) {
        idt_set_gate(i, (uint32_t)((void*[]){
            exc0_handler, exc1_handler, exc2_handler, exc3_handler,
            exc4_handler, exc5_handler, exc6_handler, exc7_handler,
            exc8_handler, exc9_handler, exc10_handler, exc11_handler,
            exc12_handler, exc13_handler, exc14_handler, exc15_handler,
            exc16_handler, exc17_handler, exc18_handler, exc19_handler
        }[i]), 0x08, 0x8E);
    }

    /* ---- 2. 中断向量 20-31（未使用的异常范围） ---- */
    for (int i = 20; i < 32; i++) {
        idt_set_gate(i, (uint32_t)isr_stub, 0x08, 0x8E);
    }

    /* ---- 3. 硬件 IRQ：PIC 重映射到 0x20-0x2F ---- */
    idt_set_gate(0x20, (uint32_t)irq0_handler,  0x08, 0x8E);
    idt_set_gate(0x21, (uint32_t)irq1_handler,  0x08, 0x8E);
    idt_set_gate(0x22, (uint32_t)irq2_handler,  0x08, 0x8E);
    idt_set_gate(0x23, (uint32_t)irq3_handler,  0x08, 0x8E);
    idt_set_gate(0x24, (uint32_t)irq4_handler,  0x08, 0x8E);
    idt_set_gate(0x25, (uint32_t)irq5_handler,  0x08, 0x8E);
    idt_set_gate(0x26, (uint32_t)irq6_handler,  0x08, 0x8E);
    idt_set_gate(0x27, (uint32_t)irq7_handler,  0x08, 0x8E);
    idt_set_gate(0x28, (uint32_t)irq8_handler,  0x08, 0x8E);
    idt_set_gate(0x29, (uint32_t)irq9_handler,  0x08, 0x8E);
    idt_set_gate(0x2A, (uint32_t)irq10_handler, 0x08, 0x8E);
    idt_set_gate(0x2B, (uint32_t)irq11_handler, 0x08, 0x8E);
    idt_set_gate(0x2C, (uint32_t)irq12_handler, 0x08, 0x8E);
    idt_set_gate(0x2D, (uint32_t)irq13_handler, 0x08, 0x8E);
    idt_set_gate(0x2E, (uint32_t)irq14_handler, 0x08, 0x8E);
    idt_set_gate(0x2F, (uint32_t)irq15_handler, 0x08, 0x8E);

    /* ---- 4. 系统调用门（int 0x80） ---- */
    /* flags=0xEE = 中断门，P=1, DPL=3
     * 进入内核时 CPU 自动清除 IF。
     * ⚠️ 注意：keyboard_get_char() 实际是纯轮询（IF=0 下直接排空
     * PS/2 状态寄存器），并不是 sti;hlt——历史注释与实现不符，已修正。 */
    idt_set_gate(0x80, (uint32_t)isr80_handler, 0x08, 0xEE);

    /* ---- 5. 加载 IDTR ---- */
    idtr.limit = sizeof(idt) - 1;   /* IDT 总大小减 1 */
    idtr.base  = (uint32_t)idt;     /* IDT 的线性地址 */
    __asm__ volatile ("lidt (%0)" : : "r"(&idtr));

    vga_write("[IDT] Initialized with 256 gates. int 0x80 installed.\n");
}
