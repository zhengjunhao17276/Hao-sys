; stage2.asm - 二级引导器（加载到 0x9000）
; 流程：A20 → int13h LBA 读 kernel.elf → E820 内存映射 → 切保护模式 →
;       32 位下解析 ELF、拷段、清 bss → 构造 Multiboot1 info → 跳内核
;
; 内核入口约定（Multiboot1）：eax=0x2BADB002，ebx=info 物理地址(0x1000)

[bits 16]
[org 0x9000]
stage2_start:

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9000
    sti

    ; ---- 1. A20 ----
    mov ax, 0x2401
    int 0x15

    ; ---- 2. 读 kernel.elf（KERNEL_LBA 起，KERNEL_SECTORS 个扇区）→ 0x20000 ----
    ; 分块读：每块 ≤63 扇区（32KB，不跨 64KB 段边界），缓冲段/ LBA 递增
    mov word [dap_kernel + 2], 63
    mov word [dap_kernel + 4], 0
    mov word [dap_kernel + 6], 0x2000
    mov dword [dap_kernel + 8], KERNEL_LBA
    mov cx, KERNEL_SECTORS
.read_loop:
    test cx, cx
    jle .read_done
    mov ax, cx
    cmp ax, 63
    jbe .read_chunk
    mov ax, 63
.read_chunk:
    mov [dap_kernel + 2], ax
    mov si, dap_kernel
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc boot_err
    sub cx, ax
    movzx eax, ax
    add dword [dap_kernel + 8], eax    ; LBA += 块大小
    movzx eax, ax
    shl eax, 5                         ; 缓冲段 += 块大小*512/16
    add word [dap_kernel + 6], ax
    jmp .read_loop
.read_done:

    ; ---- 3. E820 → multiboot mmap 条目（info 0x1000，mmap 从 0x1010 起） ----
    xor ebx, ebx
    mov dword [mmap_len], 0
    mov dword [mem_upper_kb], 0
.e820_loop:
    mov dword [e820buf], 0
    mov dword [e820buf + 4], 0
    mov dword [e820buf + 8], 0
    mov dword [e820buf + 12], 0
    mov dword [e820buf + 16], 0
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 20
    mov di, e820buf
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done

    ; 写 multiboot 条目到 mmap 区（info + 16 起）
    mov di, [mmap_write_ptr]
    mov dword [di], 20            ; size（不含自身，内核步长 = size+4 = 24）
    mov eax, [e820buf + 0]        ; base_lo
    mov [di + 4], eax
    mov eax, [e820buf + 4]        ; base_hi
    mov [di + 8], eax
    mov eax, [e820buf + 8]        ; len_lo
    mov [di + 12], eax
    mov eax, [e820buf + 12]       ; len_hi
    mov [di + 16], eax
    mov eax, [e820buf + 16]       ; type
    mov [di + 20], eax
    add dword [mmap_write_ptr], 24
    add dword [mmap_len], 24

    ; mem_upper：找 1MB 以上可用区域（type=1）的最大结束地址
    cmp dword [e820buf + 16], 1
    jne .e820_next
    mov eax, [e820buf + 8]        ; len_lo
    add eax, [e820buf + 0]        ; base_lo + len_lo = 结束地址
    cmp eax, [mem_upper_kb]
    jbe .e820_next
    mov [mem_upper_kb], eax
.e820_next:
    test ebx, ebx
    jnz .e820_loop
.e820_done:

    ; mem_upper_kb 现在是结束地址（字节）→ 转 KB（-1MB 后 /1024）
    mov eax, [mem_upper_kb]
    sub eax, 0x100000
    shr eax, 10
    mov [mem_upper_kb], eax

    ; ---- 4. 填 multiboot info（0x1000） ----
    mov dword [0x1000], 0x40      ; flags: MULTIBOOT_MMAP
    mov dword [0x1004], 640       ; mem_lower (KB)
    mov eax, [mem_upper_kb]
    mov [0x1008], eax             ; mem_upper (KB)
    mov eax, [mmap_len]
    mov [0x1000 + 44], eax        ; mmap_length
    mov dword [0x1000 + 48], 0x1010  ; mmap_addr

    ; ---- 5. 切保护模式 ----
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x9000

    ; ---- 6. 解析 ELF（blob 在 0x20000），拷 LOAD 段 ----
    mov esi, 0x20000
    mov eax, [esi + 0x1C]         ; e_phoff
    movzx ecx, word [esi + 0x2A]  ; e_phentsize
    movzx edx, word [esi + 0x2C]  ; e_phnum
    mov [phentsize_save], ecx
    add esi, eax                  ; esi = phdr 表
.phdr_loop:
    test edx, edx
    jz .load_done
    dec edx
    cmp dword [esi], 1            ; PT_LOAD？
    jne .next_phdr

    ; 拷贝 p_filesz 字节：0x20000+p_offset → p_vaddr
    mov ecx, [esi + 16]           ; p_filesz
    mov edi, [esi + 8]            ; p_vaddr
    mov eax, [esi + 4]            ; p_offset
    add eax, 0x20000
    push esi
    push edx
    push ecx
    mov esi, eax
    rep movsb
    ; 清 p_memsz - p_filesz（bss）
    pop ecx                       ; p_filesz
    pop edx
    pop esi
    mov eax, [esi + 20]           ; p_memsz
    sub eax, ecx
    jle .next_phdr
    mov edi, [esi + 8]            ; p_vaddr
    add edi, ecx
    xor eax, eax
    ; memset（按 4 字节）
    mov ecx, [esi + 20]
    sub ecx, [esi + 16]
    shr ecx, 2
    rep stosd
.next_phdr:
    add esi, [phentsize_save]
    jmp .phdr_loop

.load_done:
    ; ---- 7. 跳内核 ----
    mov eax, 0x2BADB002
    mov ebx, 0x1000
    mov ecx, [0x20018]            ; e_entry
    jmp ecx

.hang:
    hlt
    jmp .hang

[bits 16]
boot_err:
    mov si, msg_err
.print:
    lodsb
    test al, al
    jz .hang16
    mov ah, 0x0E
    int 0x10
    jmp .print
.hang16:
    hlt
    jmp .hang16
msg_err db "Kernel load error", 0

; ---- 数据 ----
align 4
dap_kernel:
    db 0x10
    db 0
    dw KERNEL_SECTORS
    dw 0x0000
    dw 0x2000                ; 段 0x2000:0 = 物理 0x20000
    dd KERNEL_LBA
    dd 0
e820buf: times 24 db 0
mmap_len: dd 0
mmap_write_ptr: dd 0x1010
mem_upper_kb: dd 0
phentsize_save: dd 0

gdt:
    dq 0
    ; code: limit 4G, base 0, access 0x9A, flags 0xCF
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
    ; data: limit 4G, base 0, access 0x92, flags 0xCF
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt
