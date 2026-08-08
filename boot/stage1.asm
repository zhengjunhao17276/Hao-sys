; stage1.asm - MBR 引导器（446 字节，装在扇区 0 的代码区，分区表在 446+）
; 职责：用 int 13h LBA 扩展读 stage2（扇区 1-2）到 0x9000，跳过去。
; 之后的事（A20、读内核、ELF 加载、切保护模式）交给 stage2。
;
; 镜像布局：
;   扇区 0        = 本 MBR（前 446 字节代码 + 分区表）
;   扇区 1-2      = stage2（1024 字节，加载到 0x9000）
;   扇区 3..      = kernel.elf 原始字节
;   扇区 2048..   = FAT16 分区（HaoOS 根文件系统）

[bits 16]
[org 0x7C00]

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; 读扇区 1-2 到 0x9000（int 13h AH=42h，LBA 扩展）
    mov si, dap
    mov ah, 0x42
    mov dl, 0x80              ; 第一块硬盘
    int 0x13
    jc .disk_error

    ; 跳 stage2
    jmp 0x0000:0x9000

.disk_error:
    mov si, msg_err
.print:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    int 0x10
    jmp .print
.hang:
    hlt
    jmp .hang

msg_err db "Boot error", 0

; 磁盘地址包（int 13h AH=42h）
align 4
dap:
    db 0x10                   ; 包大小
    db 0                      ; 保留
    dw 2                      ; 扇区数
    dw 0x9000                 ; 缓冲偏移
    dw 0                      ; 缓冲段
    dd 1                      ; 起始 LBA
    dd 0                      ; LBA 高 32 位

times 446-($-$$) db 0        ; 填到 446 字节（分区表从 446 开始，由 make_image.sh 写入）
