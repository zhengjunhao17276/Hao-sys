; switch.asm - 任务切换
;   switch_to(prev, next)      内核任务切换：pusha/popa 存恢复上下文
;   switch_to_user(prev, next) 切到用户任务：手动 pop + iret
;   iret_to_user(eip, esp)     直接 iret 进 ring3
;
; task_t 偏移 0 = esp（切换栈即切换执行流），偏移 4 = eip
; pusha 顺序：EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI；popa 反之

global switch_to
global switch_to_user
global iret_to_user

section .text

; void switch_to(task_t* prev, task_t* next)
; [ebp+8]=prev, [ebp+12]=next
; pusha 存上下文 → prev->esp = esp → 载 next->esp → popa → ret
switch_to:
    push ebp
    mov ebp, esp

    ; 上下文快照：切回时 popa 恢复，仿佛从未离开
    pusha

    mov eax, [ebp + 8]      ; prev
    mov edx, [ebp + 12]     ; next

    ; 保存的是 pusha 之后（含 8 寄存器 + EBP）的 ESP；
    ; prev 为 NULL（终止路径）时跳过
    test eax, eax
    jz .skip_save
    mov [eax], esp
.skip_save:

    ; 换栈：指向新任务上次 pusha 之后的位置
    mov esp, [edx]

    ; ⚠️ 恢复内核数据段：从被抢占的用户任务（ds=0x23）切到内核任务时
    ; 不清段寄存器会让内核带着 0x23 跑（平坦模型下侥幸能跑，非平坦
    ; 段/严格保护下必炸）
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; popa 恢复寄存器（ESP 槽被丢弃，栈指针不变）
    popa

    pop ebp

    ; 弹出 EIP：首次调度是 entry，之后是上次暂停点
    ret

; void switch_to_user(task_t* prev, task_t* next)
; 内核任务 → 用户任务（旧版只设段寄存器就 retf，是残废实现）：
;   pusha 存 prev → prev->esp = esp（prev 可为 NULL）→ 载 next->esp
;   （内核栈上 [pusha 帧][iret 帧]）→ 设用户段 → 手动 pop 寄存器 → iret
; prev 是用户任务时调用方必须传 NULL：其上下文由中断入口保存，
; 这里 pusha 会覆盖 iret 帧指针
switch_to_user:
    push ebp
    mov ebp, esp

    pusha          ; 保存 prev 上下文（prev 为内核任务时）

    mov eax, [ebp + 8]      ; prev
    mov edx, [ebp + 12]     ; next

    test eax, eax
    jz .skip_save
    mov [eax], esp          ; prev->esp = 当前栈指针
.skip_save:

    ; 换栈：next 内核栈上的 [pusha 帧][iret 帧]，esp 指向 pusha 帧顶
    ; ⚠️ 修复：必须手动 pop 寄存器再 iret——裸 iret 只恢复 EIP/CS/EFLAGS/
    ; ESP/SS。抢占发生在用户态代码中途时（如 putchar 的 mov %al,-0x1(%ebp)），
    ; EAX/EBP 全是内核残留值 → 用户态写垃圾地址 → #GP（实测抓到）
    mov esp, [edx]

    ; 切到用户数据段（ring3）
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 手动 pop 用户寄存器（顺序同 popa，ESP 槽跳过）
    pop edi
    pop esi
    pop ebp
    add esp, 4
    pop ebx
    pop edx
    pop ecx
    pop eax
    ; 此时 ESP 在 iret 帧起点：弹出 EIP/CS/EFLAGS/ESP/SS 进 ring3
    iret

; void iret_to_user(uint32_t eip, uint32_t esp)
; 在栈上现搭 iret 帧进 ring3（不经调度器）
iret_to_user:
    mov eax, [esp + 4]      ; eip
    mov edx, [esp + 8]      ; esp

    cli                     ; 避免 iret 前被中断打断

    ; iret 帧（弹出顺序 EIP,CS,EFLAGS,ESP,SS → 反着压）
    push dword 0x23      ; SS  = 用户数据段 (ring3)
    push edx             ; ESP
    push dword 0x200     ; EFLAGS (IF=1)
    push dword 0x1B      ; CS  = 用户代码段 (ring3)
    push eax             ; EIP
    iret
