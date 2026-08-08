/*
 * kmain.c - 内核 C 入口（start.asm 的 _start 调用）
 * 初始化顺序：VGA → PMM → VMM → IDT/PIC → PIT → PCI → USB →
 * 键盘/鼠标 → ATA/FAT → 任务；每步依赖前一步，顺序不能乱。
 */

#include <stdint.h>
#include <stdbool.h>

#include "./include/driver/vga.h"
#include "./include/driver/block_dev.h"
#include "./include/syscall/idt.h"
#include "./include/driver/pic.h"
#include "./include/driver/keyboard.h"
#include "./include/driver/mouse.h"
#include "./include/driver/ata.h"
#include "./include/driver/pci.h"
#include "./include/driver/usb.h"
#include "./include/driver/speaker.h"
#include "./include/syscall/syscall.h"
#include "./include/driver/rtc.h"
#include "./include/driver/pit.h"
#include "./include/mm/pmm.h"
#include "./include/mm/vmm.h"
#include "./include/proc/task.h"
#include "./include/fs/fat.h"
#include "./include/fs/vfs.h"
#include "./include/lib/string.h"

/* ata0 块设备后端 */
static bool ata_blk_read(uint32_t lba, void* buf) { return ata_read_sector(lba, (uint8_t*)buf); }
static bool ata_blk_write(uint32_t lba, const void* buf) { return ata_write_sector(lba, (const uint8_t*)buf); }
static block_dev_t ata_block_dev = { "/dev/ata0", ata_blk_read, ata_blk_write, 0 };

/* ata1（同通道从盘）：文件系统识别测试/第二块盘 */
static bool ata_blk_read_slave(uint32_t lba, void* buf) { return ata_read_sector_slave(lba, (uint8_t*)buf); }
static bool ata_blk_write_slave(uint32_t lba, const void* buf) { return ata_write_sector_slave(lba, (const uint8_t*)buf); }
static block_dev_t ata_slave_block_dev = { "/dev/ata1", ata_blk_read_slave, ata_blk_write_slave, 0 };

/* @magic/info_addr 由 GRUB 传入；返回磁盘上是否有 FAT 文件系统。
 * 顺序依赖：PCI 扫描必须在 USB 之前（USB 控制器靠 PCI 发现）。 */
bool kinit(uint32_t magic, uint32_t info_addr) {
    (void)magic;   /* 启动日志已精简，魔数/地址不再展示 */
    vga_init();

    pmm_init(magic, info_addr);
    vmm_init();

    idt_init();
    pic_init();

    /* PIT 100Hz 抢占时钟：须在 PIC 重映射（IRQ0→0x20）之后
     * 初始化，并取消 IRQ0 屏蔽。 */
    pit_init(100);
    pic_mask_irq(0, false);

    pci_init();

    usb_init();

    rtc_init();   /* 记录开机时刻（uptime 基准） */
    keyboard_init();
    mouse_init();

    bool fs_ready = false;
    if(ata_init(true)) {
        ata_block_dev.sector_count = ata_sector_count;   /* IDENTIFY 容量 */
        vfs_register_device(&ata_block_dev);
        fs_ready = vfs_mount("/", "/dev/ata0");
    }

    /* 从盘（若有）：注册为 /dev/ata1，不自动挂载 */
    if (ata_slave_sector_count > 0) {
        ata_slave_block_dev.sector_count = ata_slave_sector_count;
        vfs_register_device(&ata_slave_block_dev);
    }







    task_init();

    return fs_ready;
}

/* 汇编实现的上下文切换 */
extern void switch_to_user(task_t* next);

/* 加载 SHELL.BIN 并 iret 进 ring3。
 * 要点：代码/栈页都要 PAGE_USER；iret 帧 CS=0x1B、SS=0x23；
 * EFLAGS IF=1（用户态要能收中断）。 */
static void load_and_run_shell(void) {

    /* 1. 搜索 SHELL.BIN（Linux 布局：/bin/SHELL.BIN） */
    /* vfs_resolve 把 "/BIN/SHELL.BIN" 路由到根挂载，sub="BIN/SHELL.BIN"；
     * fat_find_file 逐段解析子目录，最终转 8.3（"SHELL   BIN"）匹配 */
    fat_dirent_t entry;
    const char* sub = NULL;
    fat_fs_t* root_fs = vfs_resolve("/BIN/SHELL.BIN", &sub);
    if (!root_fs || !fat_find_file(root_fs, sub, &entry)) {
	/* 找不到就直接返回 */
        vga_write("[Shell] /BIN/SHELL.BIN not found.\n");
        return;
    }

    /* 2. 分配代码物理页
     * ⚠️ 额外映射 64KB .bss 区（BSS_PAGES 页）：用户程序的 static
     * 大缓冲（如 FM 复制用的 64KB）落在 .bss，扁平二进制不含 .bss，
     * 不映射会 #PF。 */
    #define BSS_PAGES 24   /* .bss 上限 96KB（shell 自身静态缓冲） */
    uint32_t file_pages = (entry.file_size + 4095) / 4096;
    uint32_t code_pages = file_pages + BSS_PAGES;
    if (file_pages > 64) {
        /* 保险丝：>256KB 直接拒绝（理论上不可能） */
        vga_write("[Shell] SHELL.BIN too large (>256KB), refusing.\n");
        return;
    }
    uint32_t code_phys = (uint32_t)pmm_alloc_page();
    if (!code_phys) {
        vga_write("[Shell] Failed to allocate code page.\n");
        return;
    }

    /* 3. 虚拟地址与用户任务 */
    /* ⚠️ 用户任务有独立页目录（复制内核页表），但内核身份映射覆盖
     * 全部物理内存（128MB）——用户代码/栈只能放在内核映射区之外，
     * 固定用 256MB（0x10000000，内核未建页表）。 */
    uint32_t code_virt = 0x10000000;
    uint32_t stack_virt = code_virt + code_pages * 0x1000;

    /* task_create_user 内部创建独立页目录，后续映射全部针对其目录 */
    task_t* shell_task = task_create_user("shell", (void*)code_virt, (void*)stack_virt);
    if (!shell_task) {
        vga_write("[Shell] Failed to create user task.\n");
        pmm_free_page((void*)code_phys);
        return;
    }
    uint32_t* target_dir = shell_task->page_directory;
    if (!target_dir) {
        vga_write("[Shell] No page directory!\n");
        pmm_free_page((void*)code_phys);
        return;
    }

    /* 映射代码首页到用户目录 */
    if (!vmm_map_page(target_dir, code_virt, code_phys, PAGE_WRITE | PAGE_USER)) {
        vga_write("[Shell] Failed to map code page!\n");
        pmm_free_page((void*)code_phys);
        return;
    }

    /* ⚠️ 一次性分配所有额外页，失败时统一释放，不再泄漏 */
    uint32_t extra_phys[80];
    for (uint32_t pi = 1; pi < code_pages; pi++) {
        extra_phys[pi - 1] = (uint32_t)pmm_alloc_page();
        if (!extra_phys[pi - 1]) {
            vga_write("[Shell] Failed to allocate extra page.\n");
            for (uint32_t j = 0; j < pi - 1; j++) pmm_free_page((void*)extra_phys[j]);
            vmm_unmap_page(target_dir, code_virt);
            pmm_free_page((void*)code_phys);
            return;
        }
    }
    /* 再统一映射额外页（失败时解除全部映射并释放所有页） */
    for (uint32_t pi = 1; pi < code_pages; pi++) {
        if (!vmm_map_page(target_dir, code_virt + pi * 4096, extra_phys[pi - 1], PAGE_WRITE | PAGE_USER)) {
            vga_write("[Shell] Failed to map extra page at ");
            vga_write_hex(code_virt + pi * 4096);
            vga_write("\n");
            for (uint32_t j = 0; j < code_pages; j++) {
                uint32_t v = code_virt + j * 4096;
                if (vmm_get_phys_addr(target_dir, v) != 0) vmm_unmap_page(target_dir, v);
            }
            for (uint32_t j = 0; j < code_pages - 1; j++) pmm_free_page((void*)extra_phys[j]);
            pmm_free_page((void*)code_phys);
            return;
        }
    }

    /* ⚠️ 不能按物理地址连续写：free_list 乱序分配导致代码物理页
     * 不连续，连续写会穿到别的页（实测覆盖了 idle 的 PCB）。
     * 先在内核目录临时映射连续虚拟地址，加载完解除。 */
    uint32_t load_virt = 0x20000000;   /* 内核目录未用区域 */
    for (uint32_t pi = 0; pi < code_pages; pi++) {
        uint32_t phys = (pi == 0) ? code_phys : extra_phys[pi - 1];
        vmm_map_page(vmm_get_current_directory(), load_virt + pi * 4096, phys, PAGE_WRITE);
    }

    /* 只加载文件内容（.bss 页零填充即可） */
    uint32_t loaded = fat_load_file(root_fs, &entry, (void*)load_virt, entry.file_size);
    for (uint32_t pi = file_pages; pi < code_pages; pi++) {
        /* .bss 页清零 */
        memset((void*)(load_virt + pi * 4096), 0, 4096);
    }
    for (uint32_t pi = 0; pi < code_pages; pi++) {
        vmm_unmap_page(vmm_get_current_directory(), load_virt + pi * 4096);
    }
    if (loaded != entry.file_size) {
        vga_write("[Shell] Load failed!\n");
        while(1) __asm__ volatile("hlt");
    }

    /* 6. 用户栈 */
    /* ⚠️ 栈要 2 页：main 的 x86 序言（lea 4(%esp),%ecx; and $-16,%esp;
     * push -4(%ecx)）会读栈顶之上 4 字节——1 页栈（栈顶恰好越界）
     * 会触发 #PF。 */
    uint32_t stack_phys = (uint32_t)pmm_alloc_page();
    uint32_t stack_phys2 = (uint32_t)pmm_alloc_page();
    if (!stack_phys || !stack_phys2) {
        vga_write("[Shell] Failed to allocate stack page.\n");
        if (stack_phys) pmm_free_page((void*)stack_phys);
        if (stack_phys2) pmm_free_page((void*)stack_phys2);
        vmm_unmap_page(target_dir, code_virt);
        pmm_free_page((void*)code_phys);
        return;
    }

    if (!vmm_map_page(target_dir, stack_virt, stack_phys, PAGE_WRITE | PAGE_USER)) {
        vga_write("[Shell] Failed to map stack page at ");
        vga_write_hex(stack_virt);
        vga_write("\n");
        pmm_free_page((void*)stack_phys);
        pmm_free_page((void*)stack_phys2);
        vmm_unmap_page(target_dir, code_virt);
        pmm_free_page((void*)code_phys);
        return;
    }
    if (!vmm_map_page(target_dir, stack_virt + 0x1000, stack_phys2, PAGE_WRITE | PAGE_USER)) {
        vga_write("[Shell] Failed to map stack page2 at ");
        vga_write_hex(stack_virt + 0x1000);
        vga_write("\n");
        pmm_free_page((void*)stack_phys);
        pmm_free_page((void*)stack_phys2);
        vmm_unmap_page(target_dir, stack_virt);
        vmm_unmap_page(target_dir, code_virt);
        pmm_free_page((void*)code_phys);
        return;
    }

    /* 登记用户页供退出时回收（代码页 + 用户栈页） */
    shell_task->user_virt_count = 0;
    for (uint32_t pi = 0; pi < code_pages && shell_task->user_virt_count < 96; pi++) {
        shell_task->user_virt_pages[shell_task->user_virt_count++] = code_virt + pi * 4096;
    }
    if (shell_task->user_virt_count < 96) {
        shell_task->user_virt_pages[shell_task->user_virt_count++] = stack_virt;
    }
    if (shell_task->user_virt_count < 96) {
        shell_task->user_virt_pages[shell_task->user_virt_count++] = stack_virt + 0x1000;
    }

    /* ⚠️ 必须让调度器知道真正在跑的是 shell。之前 current_task 一直
     * 是 idle——裸 iret 进用户态后，int 0x80/IRQ 入口的
     * mov [current_task],esp 会把 shell 的中断帧写进 idle 的 PCB
     * （污染 idle->esp），一旦切回 idle 就从垃圾栈 popa → 崩溃。
     * idle 保持 RUNNING 即可：round-robin 只选 READY 任务，
     * 永远轮不到它（它也没有可执行代码）。 */
    extern task_t* current_task;
    shell_task->state = TASK_RUNNING;
    current_task = shell_task;

    /* 8. iret 切换进用户态
     * a) 先设 TSS.esp0，否则用户态 int 0x80 用 esp0=0 压栈 →
     *    页错误 → 三重重启（黑屏）
     * b) iret 帧：SS=0x23, ESP=user_stack_top, EFLAGS=0x200(IF=1),
     *    CS=0x1B, EIP=code_virt
     * c) iret 弹出帧切到 ring3
     * ⚠️ 之前 hardcode 了 EIP=0x01000000/ESP=0x01002000，但 code_virt
     * 是动态选的，现在用约束传正确值。 */

    /* (a) 用任务自己的内核栈（task_create_user 里已分配），绝不能
     * 指向用户栈顶——否则内核中断帧直接压在用户栈上踩坏栈帧
     * （实测：一按键就跳飞）。 */
    task_set_kernel_stack(shell_task->kernel_esp0);

    /* ⚠️ 切 CR3 到 shell 的独立页目录（256MB 区只映射在该目录，
     * 内核目录里没有，不切会立即 #PF）。 */
    vmm_switch_directory(shell_task->page_directory);

    /* 直接内联 IRET，不经过任何函数调用 */
    __asm__ volatile (
        "movl %0, %%eax\n"
        "movl %1, %%edx\n"
        "cli\n"
        "pushl $0x23\n"
        "pushl %%edx\n"
        "pushl $0x200\n"
        "pushl $0x1B\n"
        "pushl %%eax\n"
        "iret\n"
        :
        : "r"(code_virt), "r"((uint32_t)stack_virt + 0x1000)
        : "eax", "edx", "memory"
    );
    while (1) __asm__ volatile ("hlt");
}

/* start.asm 的 _start 调进来：kinit 后按文件系统是否就绪
 * 决定是否加载 Shell。 */
void kmain(uint32_t magic, uint32_t info_addr) {
    if(kinit(magic, info_addr)){
	load_and_run_shell();
    }
    else {
        vga_write("[Auto] No filesystem, skipping shell load.\n");
        while (1) __asm__ volatile ("hlt");   /* 修复：空闲时 HLT，别让 QEMU 宿主 CPU 100% */
    }
    /* 两个分支都不返回（load_and_run_shell 内部已 iret 进用户态） */
}
