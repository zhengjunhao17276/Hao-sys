/*
 * idt.c - IDT 初始化
 * 向量分配：0x00-0x13 CPU 异常，0x20-0x2F 硬件 IRQ（PIC 重映射），
 * 0x80 系统调用（DPL=3），其余指向 isr_stub。
 */

#include "../include/syscall/idt.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"

extern void isr_stub(void);       /* 未用中断的默认处理 */
extern void irq0_handler(void);
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
extern void isr80_handler(void);  /* int 0x80 系统调用 */

/* IDT（256 个条目 × 8 字节 = 2048 字节） */
static idt_entry_t idt[256];
static idtr_t idtr;

/* flags: bit7=P, bit6-5=DPL, bit0-3=type；0x8E=内核中断门，
 * 0xEE=用户态可触发（DPL=3） */
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags) {
    idt[num].base_low  = handler & 0xFFFF;
    idt[num].base_high = (handler >> 16) & 0xFFFF;
    idt[num].selector  = selector;
    idt[num].zero      = 0;        /* 保留位必须为 0 */
    idt[num].flags     = flags;
}

/* 256 个门全装上，最后 lidt */
void idt_init(void) {
    /* ---- 1. CPU 异常 0-19 ---- */
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

    /* 中断门 0x8E（P=1, DPL=0），仅内核态可触发 */
    for (int i = 0; i < 20; i++) {
        idt_set_gate(i, (uint32_t)((void*[]){
            exc0_handler, exc1_handler, exc2_handler, exc3_handler,
            exc4_handler, exc5_handler, exc6_handler, exc7_handler,
            exc8_handler, exc9_handler, exc10_handler, exc11_handler,
            exc12_handler, exc13_handler, exc14_handler, exc15_handler,
            exc16_handler, exc17_handler, exc18_handler, exc19_handler
        }[i]), 0x08, 0x8E);
    }

    /* ---- 2. 向量 20-31（未使用的异常范围）---- */
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

    /* ---- 4. 系统调用门（int 0x80）---- */
    /* 0xEE = DPL=3 中断门；进入内核时 CPU 自动清 IF。
     * ⚠️ keyboard_get_char() 实际是纯轮询（IF=0 下直接排空 PS/2
     * 状态寄存器），不是 sti;hlt——历史注释与实现不符，已修正。 */
    idt_set_gate(0x80, (uint32_t)isr80_handler, 0x08, 0xEE);

    /* ---- 5. 加载 IDTR ---- */
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint32_t)idt;
    __asm__ volatile ("lidt (%0)" : : "r"(&idtr));
}
