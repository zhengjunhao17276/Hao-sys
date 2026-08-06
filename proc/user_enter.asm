; user_enter.asm - 直接 iret 进 ring3
; void user_enter(uint32_t eip, uint32_t esp);

global user_enter

section .text

user_enter:
    cli                     ; 关中断，避免 iret 前被打断

    mov eax, [esp + 4]      ; eip
    mov edx, [esp + 8]      ; esp

    ; 搭 iret 帧（弹出顺序 EIP,CS,EFLAGS,ESP,SS → 反着压）
    push dword 0x23         ; SS  = 用户数据段 (ring3)
    push edx                ; ESP
    push dword 0x200        ; EFLAGS (IF=1)
    push dword 0x1B         ; CS  = 用户代码段 (ring3)
    push eax                ; EIP

    iret                    ; 切到 ring3
