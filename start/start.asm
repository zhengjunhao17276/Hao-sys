; =============================================================================
; start.asm - 内核入口点与 Multiboot 头部
;
; 这是 HaoOS 内核被执行的第一段代码。GRUB（或其他 Multiboot 兼容引导器）
; 加载内核 ELF 文件后，首先跳转到 _start 标签处执行。
;
; 这段代码做了三件事：
;   1. 声明 Multiboot 头部（在 .multiboot 段），让 GRUB 识别这是一个
;      符合 Multiboot 规范的内核。
;   2. 设置内核栈（stack_top），因为 C 语言代码需要有效的栈才能运行。
;   3. 跳转到 kmain() —— 内核的 C 语言入口点。
;
; Multiboot 头部格式（三个 32 位值）：
;   - magic:    0x1BADB002（固定魔数，引导器用来验证）
;   - flags:    0x03（bit0=需要内存信息，bit1=需要启动设备信息）
;   - checksum: -(magic + flags)（校验和，三个值加起来应为 0）
; =============================================================================

section .multiboot
align 4
    dd 0x1BADB002          ; Multiboot 魔数：固定值，引导器据此识别
    dd 0x03                ; flags：bit0 请求引导器提供内存布局信息
    dd -(0x1BADB002 + 0x03) ; checksum：magic + flags + checksum = 0

section .text
global _start              ; 对外导出 _start，链接脚本通过 ENTRY(_start) 引用
extern kmain               ; 声明 C 函数 kmain，链接器会解析其地址

_start:
    ; ESP ← stack_top：初始化内核栈指针
    ; 栈是向下增长的，stack_top 是 16KB 栈空间的末尾地址
    mov esp, stack_top

    ; 将 Multiboot 信息传递给 kmain
    ; Multiboot 规范约定：eax = 魔数 0x2BADB002，ebx = multiboot_info_t 结构地址
    push ebx               ; ebx → info_addr（第二个参数）
    push eax               ; eax → magic（第一个参数）
    call kmain             ; 调用 kmain(magic, info_addr)

    ; kmain 返回后不应该继续执行，但为了防止意外：
    cli                    ; 关中断（关掉所有可屏蔽硬件中断）
.hang:
    hlt                    ; 让 CPU 进入低功耗暂停状态
    jmp .hang              ; 无限循环，防止执行到未定义区域

section .bss
align 16
    resb 16384             ; 预留 16KB 空间作为内核栈
stack_top:                 ; 栈顶标签：在预留空间的末尾
