; =============================================================================
; isr_asm.asm - 中断服务例程（异常、IRQ、系统调用）
;
; 本文件使用 NASM 宏批量生成中断处理程序，以减少重复代码。
;
; 异常处理（Exception）：
;   每个异常处理程序在 VGA 终端打印 "EX:XX"（XX 是异常号），
;   然后进入无限 HLT 循环。异常意味着内核有 bug 或硬件问题，
;   不应继续执行。
;
; IRQ 处理：
;   IRQ 是硬件中断请求，由 PIC 转发到 CPU。本驱动器特殊处理
;   IRQ1（键盘）和 IRQ12（鼠标），转发给 C 函数处理，其余 IRQ
;   直接发送 EOI。
;
; 系统调用（int 0x80）：
;   保存所有寄存器后调用 syscall_dispatcher()，完成后恢复并返回。
;   这是用户态与内核通信的唯一通道。
; =============================================================================

extern keyboard_irq_handler
extern ps2_mouse_irq_handler
extern pic_send_eoi
extern pic_is_in_service
extern pit_tick_handler
extern syscall_dispatcher
extern current_task

section .text

; =============================================================================
; 异常处理宏：EXCEPTION num
; 生成一个处理特定 CPU 异常的汇编函数。
; CPU 异常包括除零错（#DE, 0）、一般保护错误（#GP, 13）、
; 页错误（#PF, 14）等。
;
; 发生异常时：
;   1. 丢弃 CPU 压入的错误码（#8/#10-#14/#17 才有）
;   2. 用 pusha 保存所有通用寄存器
;   3. 在 VGA 终端左上角打印 "EX:XX" + 栈上几个关键值（含 EIP）
;   4. 进入无限 HLT（异常意味着内核 bug，不应继续执行）
;
; ⚠️ 错误码异常（#DB 除外）：#8 双重故障、#10 无效 TSS、#11 段不存在、
;    #12 栈段错误、#13 一般保护、#14 页错误、#17 对齐检查 会由 CPU
;    在压入 EIP/CS/EFLAGS 之前先压入一个 4 字节错误码。
;    若不丢弃，pusha 后的栈偏移全部错位——打印的 "EIP" 会读到错误码。
;    页错误（#PF）时尤其重要：EIP 错一个字节都定位不到真正的出错指令。
;
; 统一后的栈布局（从低到高，相对 pusha 后的 esp）：
;   pusha 压入的 8 个 regs (32 字节)
;   EIP   (4 字节)  ← 偏移 32：异常发生时正在执行的指令地址
;   CS    (4 字节)  ← 偏移 36
;   EFLAGS(4 字节)  ← 偏移 40
;   （若从 ring3 触发）用户 ESP ← 偏移 44
; =============================================================================
%macro EXCEPTION 2   ; %1 = 异常号, %2 = 是否压入错误码 (1=是, 0=否)
global exc%1_handler
exc%1_handler:
    %if %2
        ; 该异常带错误码：先丢弃，统一后续栈偏移
        add esp, 4
    %endif
    pusha

    ; ---- 打印 "EX:XX" ----
    mov eax, 0xB8000
    mov byte [eax], 'E'     ; E
    mov byte [eax+1], 0x0F
    mov byte [eax+2], 'X'   ; X
    mov byte [eax+3], 0x0F
    mov byte [eax+4], ':'   ; :
    mov byte [eax+5], 0x0F
    add eax, 6

    mov ebx, %1
    mov ecx, 2
    ; ⚠️ 修复：取 bl（低字节）打印异常号——旧代码用 bh，
    ; 任何 ≥1 的异常都会显示成 "EX:00"，误导调试。
    ; 高位 nibble 先打印
    mov dl, bl
    shr dl, 4
    add dl, '0'
    cmp dl, '9'
    jle .d1
    add dl, 7
.d1:
    mov byte [eax], dl
    mov byte [eax+1], 0x0F
    add eax, 2
    ; 低位 nibble
    mov dl, bl
    and dl, 0x0F
    add dl, '0'
    cmp dl, '9'
    jle .d2
    add dl, 7
.d2:
    mov byte [eax], dl
    mov byte [eax+1], 0x0F
    add eax, 2

    ; ---- 打印栈偏移 28,32,36,40,44 的原始值 ----
    ; pusha 后期望布局：EAX(28), EIP(32), CS(36), EFLAGS(40), ???(44)
    ; 用 esi 基址 + 手动展开 5 次，避免预处理器循环的标签冲突

    mov esi, esp

    ; 偏移 28
    mov byte [eax], ' '
    mov byte [eax+1], 0x0F
    add eax, 2
    mov ebx, [esi+28]
    mov ecx, 8
.s28l: rol ebx, 4
    mov dl, bl
    and dl, 0x0F
    add dl, '0'
    cmp dl, '9'
    jle .s28d
    add dl, 7
.s28d: mov byte [eax], dl
    mov byte [eax+1], 0x0F
    add eax, 2
    loop .s28l

    ; 偏移 32
    mov byte [eax], ' '
    mov byte [eax+1], 0x0F
    add eax, 2
    mov ebx, [esi+32]
    mov ecx, 8
.s32l: rol ebx, 4
    mov dl, bl
    and dl, 0x0F
    add dl, '0'
    cmp dl, '9'
    jle .s32d
    add dl, 7
.s32d: mov byte [eax], dl
    mov byte [eax+1], 0x0F
    add eax, 2
    loop .s32l

    ; 偏移 36
    mov byte [eax], ' '
    mov byte [eax+1], 0x0F
    add eax, 2
    mov ebx, [esi+36]
    mov ecx, 8
.s36l: rol ebx, 4
    mov dl, bl
    and dl, 0x0F
    add dl, '0'
    cmp dl, '9'
    jle .s36d
    add dl, 7
.s36d: mov byte [eax], dl
    mov byte [eax+1], 0x0F
    add eax, 2
    loop .s36l

    ; 偏移 40
    mov byte [eax], ' '
    mov byte [eax+1], 0x0F
    add eax, 2
    mov ebx, [esi+40]
    mov ecx, 8
.s40l: rol ebx, 4
    mov dl, bl
    and dl, 0x0F
    add dl, '0'
    cmp dl, '9'
    jle .s40d
    add dl, 7
.s40d: mov byte [eax], dl
    mov byte [eax+1], 0x0F
    add eax, 2
    loop .s40l

    ; 偏移 44
    mov byte [eax], ' '
    mov byte [eax+1], 0x0F
    add eax, 2
    mov ebx, [esi+44]
    mov ecx, 8
.s44l: rol ebx, 4
    mov dl, bl
    and dl, 0x0F
    add dl, '0'
    cmp dl, '9'
    jle .s44d
    add dl, 7
.s44d: mov byte [eax], dl
    mov byte [eax+1], 0x0F
    add eax, 2
    loop .s44l

    ; ---- halt ----
.hang:
    hlt
    jmp .hang
%endmacro

; 批量定义异常 0-19 的处理程序
; 第二个参数 = 是否压入错误码（8,10,11,12,13,14,17 为 1）
EXCEPTION 0, 0
EXCEPTION 1, 0
EXCEPTION 2, 0
EXCEPTION 3, 0
EXCEPTION 4, 0
EXCEPTION 5, 0
EXCEPTION 6, 0
EXCEPTION 7, 0
EXCEPTION 8, 1
EXCEPTION 9, 0
EXCEPTION 10, 1
EXCEPTION 11, 1
EXCEPTION 12, 1
EXCEPTION 13, 1
EXCEPTION 14, 1
EXCEPTION 15, 0
EXCEPTION 16, 0
EXCEPTION 17, 1
EXCEPTION 18, 0
EXCEPTION 19, 0

; =============================================================================
; IRQ 处理宏：IRQ num, pic_irq_num
; 生成一个处理特定硬件中断的汇编函数。
;
; 特殊处理的 IRQ：
;   IRQ1  → 键盘中断（调用 keyboard_irq_handler）
;   IRQ12 → 鼠标中断（调用 ps2_mouse_irq_handler）
;   其他  → 直接发送 EOI 后返回
;
; CPU 接收到 IRQ 后会自动在栈上压入：
;   SS, ESP, EFLAGS, CS, EIP（如果从用户态中断）
;   或 EFLAGS, CS, EIP（如果从内核态中断）
;
; iret 指令会弹出这些值并恢复之前的执行流。
; =============================================================================
%macro IRQ 2
global irq%1_handler
irq%1_handler:
    ; ⚠️ 同 isr80_handler：先 pusha 再保存 task->esp（指向 pusha 帧，
    ; iret 帧在正下方）。恢复时 switch_to_user 手动 pop + iret，
    ; 通用寄存器不再丢失。
    pusha
    mov eax, [current_task]
    test eax, eax
    jz %%nosave
    mov [eax], esp
    %%nosave:
    %if %1 == 0
        ; pusha 后栈布局：内核态 CS@[esp+36]（=0x08），
        ; 用户态 [esp+36] = 用户 ESP（栈地址，不会是 0x08）。
        ; EOI 由 pit_tick_handler 内部发送（必须在 schedule 之前）。
        cmp dword [esp+36], 0x08
        je %%kernel_ctx
        push dword 1
        jmp %%call_tick
    %%kernel_ctx:
        push dword 0
    %%call_tick:
        call pit_tick_handler
        add esp, 4
    %elif %1 == 1
        ; IRQ1 = PS/2 键盘
        call keyboard_irq_handler
    %elif %1 == 7
        ; ⚠️ 伪中断检测：IRQ7 是伪中断（Spurious）的高发线，
        ; ISR 位未置位说明是伪中断，此时发 EOI 会误清真实中断。
        push dword 7
        call pic_is_in_service
        add esp, 4
        test eax, eax
        jz %%spurious7
        push dword 7
        call pic_send_eoi
        add esp, 4
%%spurious7:
    %elif %1 == 12
        ; IRQ12 = PS/2 鼠标
        call ps2_mouse_irq_handler
    %elif %1 == 15
        ; ⚠️ 伪中断检测：IRQ15（从片）同理，从片 ISR 位未置位 = 伪中断
        push dword 15
        call pic_is_in_service
        add esp, 4
        test eax, eax
        jz %%spurious15
        push dword 15
        call pic_send_eoi
        add esp, 4
%%spurious15:
    %else
        ; 其他 IRQ：直接发送 EOI 并返回
        push dword %2
        call pic_send_eoi
        add esp, 4
    %endif
    ; 手动弹出 pusha 的寄存器（避免 popa + iret 的栈偏移）
    pop edi
    pop esi
    pop ebp
    add esp, 4
    pop ebx
    pop edx
    pop ecx
    pop eax
    iret
%endmacro

; 批量定义 IRQ 0-15 的处理程序
IRQ 0, 0
IRQ 1, 1
IRQ 2, 2
IRQ 3, 3
IRQ 4, 4
IRQ 5, 5
IRQ 6, 6
IRQ 7, 7
IRQ 8, 8
IRQ 9, 9
IRQ 10, 10
IRQ 11, 11
IRQ 12, 12
IRQ 13, 13
IRQ 14, 14
IRQ 15, 15

; =============================================================================
; isr_stub - 默认中断处理程序（用于未初始化的中断）
;
; 当发生一个没有专门处理程序的中断时（如 0x20-0x2F 之外的其他向量），
; 输出 "EX:??" 表示未知中断，向主片和从片都发送 EOI，然后返回。
;
; 这个处理程序不会死循环——因为误触发的中断可能是硬件伪中断，
; 返回比死循环更好（至少系统还能继续运行）。
; =============================================================================
global isr_stub
isr_stub:
    pusha
    ; 显示未知中断标记
    mov eax, 0xB8000
    mov word [eax], 0x0F00 | 'E'
    mov word [eax+2], 0x0F00 | 'X'
    mov word [eax+4], 0x0F00 | ':'
    mov word [eax+6], 0x0F00 | '?'
    mov word [eax+8], 0x0F00 | '?'
    ; 保守地发送两个 EOI（主片 + 从片）
    push 0x20
    call pic_send_eoi
    add esp, 4
    push 0x28
    call pic_send_eoi
    add esp, 4
    popa
    iret

; =============================================================================
; isr80_handler - 系统调用处理程序（int 0x80）
;
; 系统调用的入口点。用户态程序执行 "int $0x80" 时，CPU 会：
;   1. 从 TSS 加载内核栈（SS0/ESP0）→ 切换到内核栈
;   2. 压入用户态的 SS, ESP, EFLAGS, CS, EIP
;   3. 跳转到 isr80_handler（CS=0x08, EIP=isr80_handler）
;
; 处理流程：
;   1. pusha 保存通用寄存器
;   2. 保存段寄存器 DS, ES, FS, GS
;   3. 将当前 ESP（regs_t 结构体指针）作为参数传给 syscall_dispatcher
;   4. 调用 syscall_dispatcher（C 函数，进行具体分发）
;   5. 恢复段寄存器
;   6. popa 恢复通用寄存器（eax 中包含 syscall 返回值）
;   7. iret 返回用户态
;
; 注意：regs_t 结构体的布局必须与 pusha + push ds/es/fs/gs 的顺序一致。
; =============================================================================
global isr80_handler
isr80_handler:
    ; ⚠️ 保存用户任务上下文：task->esp 指向 pusha 帧（iret 帧在其正下方）。
    ; 恢复时（switch_to_user）手动 pop 寄存器后 iret——裸 iret 只恢复
    ; EIP/CS/EFLAGS/ESP/SS，通用寄存器会丢失（实测：抢占发生在 putchar
    ; 中途，EAX 里的字符被内核残留值覆盖 → 用户态写垃圾地址 → #GP）。
    pusha                       ; push EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    mov eax, [current_task]
    test eax, eax
    jz .no_save
    mov [eax], esp
.no_save:
    push ds                     ; 保存数据段寄存器
    push es
    push fs
    push gs
    push esp                    ; 将当前栈指针（regs_t*）作为参数传递
    call syscall_dispatcher
    add esp, 4                  ; 清理参数
    pop gs
    pop fs
    pop es
    pop ds
    ; 手动弹出 pusha 的寄存器（代替 popa，跳过 ESP 槽避免破坏栈指针）
    ; 当前 ESP = IRET_PTR - 32（指向 pusha 的 EDI）
    pop edi                     ; 弹出 EDI
    pop esi                     ; 弹出 ESI
    pop ebp                     ; 弹出 EBP
    add esp, 4                  ; 跳过保存的 ESP（不需要恢复 ESP_orig）
    pop ebx                     ; 弹出 EBX
    pop edx                     ; 弹出 EDX
    pop ecx                     ; 弹出 ECX
    pop eax                     ; 弹出 EAX（syscall 返回值）
    ; 现在 ESP = IRET_PTR，正好指向 IRET 帧的 EIP 位置
    iret                        ; 返回用户态
