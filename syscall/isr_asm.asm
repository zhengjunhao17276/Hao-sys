; isr_asm.asm - 异常/IRQ/系统调用处理（NASM 宏批量生成）
;
; 异常：打印 "EX:XX" 后死循环（异常 = 内核 bug，不应继续执行）
; IRQ：IRQ1 键盘、IRQ12 鼠标转 C 处理，IRQ7/15 查伪中断，其余直接 EOI
; int 0x80：保存全部寄存器后调 syscall_dispatcher

extern keyboard_irq_handler
extern ps2_mouse_irq_handler
extern pic_send_eoi
extern pic_is_in_service
extern pit_tick_handler
extern syscall_dispatcher
extern current_task

section .text

; EXCEPTION num, has_error_code
; 带错误码的异常（8/10/11/12/13/14/17）CPU 会先压 4 字节错误码，
; 必须丢弃，否则 pusha 后栈偏移错位——打印的 "EIP" 会读到错误码，
; 页错误时 EIP 错一个字节都定位不到出错指令。
; pusha 后栈布局（esp 起）：8 个 regs(32B) / EIP@32 / CS@36 / EFLAGS@40
; （ring3 触发时还有用户 ESP@44）
%macro EXCEPTION 2   ; %1 = 异常号, %2 = 是否压入错误码 (1=是, 0=否)
global exc%1_handler
exc%1_handler:
    %if %2
        ; 带错误码：先丢弃，统一后续栈偏移
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
    ; ⚠️ 用 bl 打印异常号——旧代码用 bh，任何 ≥1 的异常都显示成
    ; "EX:00"，误导调试。
    ; 高位 nibble
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

    ; ---- 打印 [esp+28..44] 的原始值（含 EIP）----
    ; 手动展开 5 次，避免宏内循环的标签冲突

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

; 批量定义异常 0-19；第二个参数 = 是否压入错误码
; （8,10,11,12,13,14,17 为 1）
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

; IRQ num, pic_irq_num
; IRQ1 键盘、IRQ12 鼠标转 C 处理；IRQ7/15 先查 ISR 位防伪中断；
; 其余直接 EOI。iret 帧由 CPU 自动压（用户态中断多 SS/ESP）。
%macro IRQ 2
global irq%1_handler
irq%1_handler:
    ; ⚠️ 同 isr80_handler：先 pusha 再存 task->esp（指向 pusha 帧，
    ; iret 帧在正下方）。恢复时 switch_to_user 手动 pop + iret，
    ; 通用寄存器不再丢失。
    pusha
    mov eax, [current_task]
    test eax, eax
    jz %%nosave
    mov [eax], esp
    %%nosave:
    %if %1 == 0
        ; pusha 后 [esp+36]：内核态 = CS(0x08)，用户态 = 用户 ESP
        ; （栈地址，不会是 0x08）。EOI 由 pit_tick_handler 内部发
        ; （必须在 schedule 之前）。
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
        ; ⚠️ IRQ7 伪中断高发：ISR 位未置位说明是伪中断，
        ; 此时发 EOI 会误清真实中断。
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
        ; ⚠️ IRQ15（从片）同理：从片 ISR 位未置位 = 伪中断
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
        ; 其他 IRQ：直接 EOI 返回
        push dword %2
        call pic_send_eoi
        add esp, 4
    %endif
    ; 手动弹出 pusha 的寄存器（避免 popa + iret 的栈偏移问题）
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

; 批量定义 IRQ 0-15
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

; isr_stub - 未知中断：打印 "EX:??"，主从片都发 EOI 后返回。
; 不死循环——误触发可能是硬件伪中断，返回比死循环好。
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
    ; 保守地发两个 EOI（主片 + 从片）
    push 0x20
    call pic_send_eoi
    add esp, 4
    push 0x28
    call pic_send_eoi
    add esp, 4
    popa
    iret

; isr80_handler - int 0x80 入口。
; 用户态 int 0x80 时 CPU 经 TSS 换到内核栈，压入 SS/ESP/EFLAGS/CS/EIP。
; 流程：pusha → 存 task->esp → 压段寄存器 → 把 regs_t* 传给
; syscall_dispatcher → 恢复 → popa（eax=返回值）→ iret。
; ⚠️ regs_t 的字段顺序必须与这里的压栈顺序一致。
global isr80_handler
isr80_handler:
    ; ⚠️ 保存用户任务上下文：task->esp 指向 pusha 帧（iret 帧在正下方）。
    ; 裸 iret 只恢复 EIP/CS/EFLAGS/ESP/SS，通用寄存器会丢（实测：
    ; 抢占发生在 putchar 中途，EAX 被内核残留值覆盖 → 用户态写垃圾
    ; 地址 → #GP）。
    pusha
    mov eax, [current_task]
    test eax, eax
    jz .no_save
    mov [eax], esp
.no_save:
    push ds                     ; 保存段寄存器
    push es
    push fs
    push gs
    push esp                    ; 当前栈指针即 regs_t*，作参数传入
    call syscall_dispatcher
    add esp, 4                  ; 清理参数
    pop gs
    pop fs
    pop es
    pop ds
    ; 手动弹出 pusha 的寄存器（跳过 ESP 槽，popa 会破坏栈指针）
    pop edi
    pop esi
    pop ebp
    add esp, 4
    pop ebx
    pop edx
    pop ecx
    pop eax                     ; eax = syscall 返回值
    ; 现在 ESP 正好指向 IRET 帧的 EIP 位置
    iret                        ; 返回用户态
