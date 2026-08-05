; =============================================================================
; user_enter.asm - 通过 IRET 切换到用户态
;
; void user_enter(uint32_t eip, uint32_t esp);
; =============================================================================

global user_enter

section .text

user_enter:
    ; 立即关中断，不让任何中断干扰
    cli

    ; 从栈上读参数
    mov eax, [esp + 4]     ; eax = eip (用户代码入口)
    mov edx, [esp + 8]     ; edx = esp (用户栈指针)

    ; 构造 IRET 帧
    push dword 0x23         ; SS = 用户数据段 (ring 3)
    push edx                ; ESP
    push dword 0x200        ; EFLAGS (IF=1)
    push dword 0x1B         ; CS = 用户代码段 (ring 3)
    push eax                ; EIP

    ; 执行 IRET，切换到 ring 3
    iret
