; start.asm - 内核入口：Multiboot 头 + 建栈 + 跳 kmain
; magic=0x1BADB002 固定魔数，flags bit0/bit1 要内存和启动设备信息，
; checksum 使三者之和为 0。

section .multiboot
align 4
    dd 0x1BADB002          ; Multiboot 魔数：固定值，引导器据此识别
    dd 0x03                ; flags：bit0 请求引导器提供内存布局信息
    dd -(0x1BADB002 + 0x03) ; checksum：magic + flags + checksum = 0

section .text
global _start              ; 对外导出 _start，链接脚本通过 ENTRY(_start) 引用
extern kmain               ; 声明 C 函数 kmain，链接器会解析其地址

_start:
    ; 栈向下增长，stack_top 是 16KB 栈区末尾
    mov esp, stack_top

    ; 按 Multiboot 约定传参：eax=魔数 0x2BADB002，ebx=info 结构地址
    push ebx               ; 第二个参数 info_addr
    push eax               ; 第一个参数 magic
    call kmain

    ; kmain 不应返回，兜底挂起
    cli
.hang:
    hlt
    jmp .hang

section .bss
align 16
    resb 16384             ; 16KB 内核栈
stack_top:
