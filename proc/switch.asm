; =============================================================================
; switch.asm - 任务上下文切换（汇编实现）
;
; 本文件实现两个核心函数：
;
; 1. switch_to(task_t* prev, task_t* next) —— 内核任务切换
;    使用 pusha/popa 保存和恢复所有通用寄存器，保存当前栈指针到
;    prev->esp，然后加载 next->esp 并从新栈恢复寄存器。最后 ret
;    会弹出下一个任务的 EIP，从而实现无缝切换到不同执行流。
;
; 2. switch_to_user(task_t* next) —— 切换到用户态任务
;    设置用户态的数据段（DS/ES/FS/GS = 0x23），然后通过 retf
;    （远返回）跳到用户代码段（CS=0x1B）的入口地址。
;
; task_t 结构体布局（与 proc/task.h 中定义一致）：
;   偏移 0: esp（栈指针，这是切换的核心——切换栈即切换执行流）
;   偏移 4: eip（指令指针，新任务的入口或断点）
;
; x86 pusha/popa 对应的寄存器顺序：
;   pusha: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
;   popa:  EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX
; =============================================================================

global switch_to
global switch_to_user
global iret_to_user

section .text

; =============================================================================
; void switch_to(task_t* prev, task_t* next)
;
; C 调用约定参数：
;   [ebp+8]  = prev (当前任务指针)
;   [ebp+12] = next (目标任务指针)
;
; 步骤：
;   1. pusha 保存当前任务的所有通用寄存器到它的栈上
;   2. 将当前栈指针 ESP 保存到 prev->esp
;   3. 从 next->esp 加载新栈指针
;   4. popa 恢复新任务的寄存器状态
;   5. ret 弹出新任务的 EIP（即新任务此前执行到的位置）
; =============================================================================
switch_to:
    ; 保存调用者的 EBP（标准函数序言）
    push ebp
    mov ebp, esp

    ; pusha 将所有通用寄存器压入当前任务的栈。
    ; 这是当前任务的"上下文快照"——当未来再次切换回来时，
    ; popa 会恢复这些值，仿佛从未离开过。
    pusha

    ; eax = prev（当前任务）
    mov eax, [ebp + 8]
    ; edx = next（目标任务）
    mov edx, [ebp + 12]

    ; [eax] 即 prev->esp = 当前栈指针
    ; 注意：现在栈顶有 pusha 压入的 8 个寄存器 + EBP，
    ; 所以保存的是 pusha 之后的 ESP 值。
    ; prev 为 NULL 时（终止路径，prev 已释放）跳过保存。
    test eax, eax
    jz .skip_save
    mov [eax], esp
.skip_save:

    ; ESP = next->esp：加载新任务的栈指针。
    ; 此时栈指针指向新任务上次 pusha 之后的位置
    mov esp, [edx]

    ; popa 恢复新任务的寄存器状态（顺序：EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX）
    ; 注意 popa 恢复的 ESP 值会被丢弃（栈指针不变），
    ; 其他 7 个寄存器从新栈恢复
    popa

    ; 恢复 EBP
    pop ebp

    ; ret：弹出新任务栈顶的 EIP，跳转到新任务继续执行。
    ; 对第一次被调度的新任务，EIP 指向 entry 函数
    ret

; =============================================================================
; void switch_to_user(task_t* prev, task_t* next)
;
; 切换到用户态任务。完整版实现（旧版只设置段寄存器就 retf，是残废实现）：
;
;   1. pusha 保存 prev（内核任务）的上下文——像 switch_to 一样
;   2. prev->esp = 当前栈指针（prev 为 NULL 时跳过）
;   3. 加载 next->esp —— 指向 next 内核栈上的 iret 帧
;      （该帧由 task_create_user 布置，或由 isr80_handler/IRQ 入口保存）
;   4. 设置用户数据段（DS/ES/FS/GS = 0x23）
;   5. iret —— 弹出 EIP/CS/EFLAGS/ESP/SS，进入 ring 3
;
; 注意：用户任务（prev 是用户任务时）的 esp 由中断入口保存，
; 不能在这里用 pusha 的栈覆盖——调用方应传 NULL。
; =============================================================================
switch_to_user:
    push ebp
    mov ebp, esp

    ; 保存 prev 的通用寄存器（如果 prev 是内核任务）
    pusha

    mov eax, [ebp + 8]      ; prev
    mov edx, [ebp + 12]     ; next

    test eax, eax
    jz .skip_save
    mov [eax], esp          ; prev->esp = 当前栈指针
.skip_save:

    ; 加载 next 的栈指针——指向其内核栈上的 iret 帧
    ; ⚠️ 修复：约定 task->esp = iret 帧起点 - 4（中断入口在 push eax
    ; 之后保存 esp，天然差 4 字节；task_create_user 也用占位字对齐）。
    ; 这里必须 add esp,4 对准 EIP 槽，否则 iret 会把 EIP 弹成
    ; 保存的 EAX（系统调用号）→ 跳飞。
    mov esp, [edx]
    add esp, 4

    ; 设置用户数据段选择子（ring 3）
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; iret 弹出：EIP, CS, EFLAGS, ESP, SS → 进入用户态
    iret

; =============================================================================
; void iret_to_user(uint32_t eip, uint32_t esp)
;
; 通过 iret 从 ring 0 切换到 ring 3 执行用户代码。
; 在栈上构造 iret 帧并执行 iret。
; =============================================================================

iret_to_user:
    ; C 调用约定参数：[esp+4]=eip, [esp+8]=esp
    mov eax, [esp + 4]
    mov edx, [esp + 8]

    ; 关中断，防止 CPU 响应 PIT/键盘中断干扰
    cli

    ; 构造 iret 帧
    push dword 0x23      ; SS
    push edx             ; ESP（调用者传入的用户栈顶）
    push dword 0x200     ; EFLAGS (IF=1)
    push dword 0x1B      ; CS
    push eax             ; EIP（调用者传入的代码入口）
    iret
