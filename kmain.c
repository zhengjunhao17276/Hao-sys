/**
 * =========================================================================
 * kmain.c - HaoOS 内核主入口
 *
 * 这是内核 C 语言层面的入口点，由 start.asm 中的 _start 调用。
 * HaoOS 的启动流程完整呈现了一个 i386 内核从零到能加载用户程序
 * 的全过程——读者可以跟随这个文件理解操作系统初始化各阶段的意义。
 *
 * 启动顺序（为什么是这个顺序？）：
 *   1. VGA 输出 → 先有终端才能输出调试信息
 *   2. PMM → 物理内存是其他所有子系统的基石
 *   3. VMM → 分页建立后才有虚拟地址空间的概念
 *   4. IDT/PIC → 中断系统是所有设备驱动的前提
 *   5. PCI → 总线扫描找到所有设备
 *   6. USB → USB 控制器初始化
 *   7. 键盘/鼠标 → 输入设备尽早可用
 *   8. ATA/FAT → 从磁盘读取 Shell 程序
 *   9. 进程管理 → 创建用户态任务来运行 Shell
 *
 * 每个步骤都依赖前一步的成果——这就是内核初始化的"依赖链"。
 * =========================================================================
 */

#include <stdint.h>
#include <stdbool.h>

#include "./include/driver/vga.h"
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
#include "./include/lib/string.h"

/**
 * kinit - 内核初始化（各子系统依次启动）
 * @magic:     Multiboot 魔数，应为 0x2BADB002，由 GRUB 传入
 * @info_addr: Multiboot 信息结构体物理地址，由 GRUB 填入
 *
 * 返回值：fs_ready = 磁盘上是否有可识别的 FAT 文件系统。
 * 这个值决定了后续是否尝试加载 SHELL.BIN。
 *
 * 注意：这个函数的每个步骤都有严格的顺序依赖——例如 PCI 扫描必须在
 * USB 初始化之前，因为 USB 控制器是通过 PCI 总线发现的。
 */
bool kinit(uint32_t magic, uint32_t info_addr) {
    vga_init();
    vga_write("Multiboot magic: ");
    vga_write_hex(magic);
    vga_write("   info_addr: ");
    vga_write_hex(info_addr);
    vga_write("\n\n");

    vga_write("[STEP 2] PMM init...\n");
    pmm_init(info_addr);
    vga_write("[STEP 3] VMM init...\n");
    vmm_init();

    vga_write("[STEP 4] IDT/PIC init...\n");
    idt_init();
    pic_init();

    /* PIT 定时器：抢占式调度时钟（100Hz，10ms/tick）。
     * 需在 PIC 重映射（IRQ0→0x20）之后初始化，并取消 IRQ0 屏蔽。 */
    pit_init(100);
    pic_mask_irq(0, false);

    vga_write("[STEP 5] PCI scan...\n");
    pci_init();

    vga_write("[STEP 6] USB init...\n");
    usb_init();

    vga_write("[STEP 7] Keyboard/mouse init...\n");


    rtc_init();   /* 记录开机时刻（uptime 基准） */
    keyboard_init();
    mouse_init();

    vga_write("[STEP 8] ATA init...\n");
    bool fs_ready = false;
    if(ata_init(true)) {
        vga_write("[STEP 8b] FAT init...\n");
        fs_ready = fat_init();

    }

    vga_write("[STEP 9] Task init...\n");





    task_init();

    return fs_ready;
}

/* 声明外部汇编函数 */
extern void switch_to_user(task_t* next);

/**
 * load_and_run_shell - 从磁盘加载 SHELL.BIN 并切换到用户态执行
 *
 * 这是内核第一次进入用户态的关键函数。流程如下：
 *
 *   FAT 查找文件 → 分配物理页 → 虚拟地址映射 → FAT 加载数据
 *   → 写入测试代码 → 分配用户栈 → 创建用户任务 → iret 切换到 ring 3
 *
 * 技术要点：
 *   - 代码页需要 PAGE_USER 标志，否则用户态执行时会触发页错误
 *   - 用户栈页也需要 PAGE_USER 标志
 *   - iret 帧的 CS=0x1B（GDT[3] + RPL=3），SS=0x23（GDT[4] + RPL=3）
 *   - EFLAGS 的 IF 位开启，确保用户态可以接收中断
 */
static void load_and_run_shell(void) {

    vga_write("[Shell] Free pages: ");
    vga_write_hex(pmm_get_free_pages());
    vga_write("\n");

    /* ========== 1. 搜索 SHELL.BIN 文件 ========== */
    /* FAT 文件系统使用 8.3 短文件名格式，在根目录中查找。
     * fat_find_file 会将 "SHELL.BIN" 转为 "SHELL   BIN"（8+3 填充空格）后匹配。 */
    fat_dirent_t entry;
    if (!fat_find_file("SHELL.BIN", &entry)) {
	/* 文件不存在则直接返回，打印提示信息 */
        vga_write("[Shell] SHELL.BIN not found.\n");
        return;
    }
    /* 找到文件：输出文件大小（字节数），便于调试 */
    vga_write("[Shell] Found SHELL.BIN, size=");
    vga_write_hex(entry.file_size);
    vga_write(" bytes.\n");

    /* ========== 2. 分配 Shell 代码所需的物理页 ========== */
    /* 一个页 4KB，需要多页存放完整 Shell */
    uint32_t code_pages = (entry.file_size + 4095) / 4096;
    if (code_pages > 64) {
        /* 保险丝：SHELL.BIN 超过 256KB 直接拒绝（理论上不可能） */
        vga_write("[Shell] SHELL.BIN too large (>256KB), refusing.\n");
        return;
    }
    uint32_t code_phys = (uint32_t)pmm_alloc_page();
    if (!code_phys) {
        vga_write("[Shell] Failed to allocate code page.\n");
        return;
    }
    vga_write("[Shell] Code phys=");
    vga_write_hex(code_phys);
    vga_write(" pages=");
    vga_write_hex(code_pages);
    vga_write("\n");

    /* ========== 3. 选择虚拟地址并创建用户任务 ========== */
    /* ⚠️ 架构升级（地址空间隔离）：用户任务有独立页目录（复制内核页表），
     * 但内核身份映射覆盖全部物理内存（128MB）——用户代码/栈只能映射到
     * 内核映射区之外。固定用 256MB（0x10000000，内核未建页表）。 */
    uint32_t code_virt = 0x10000000;
    uint32_t stack_virt = code_virt + code_pages * 0x1000;

    /* 先创建用户任务（task_create_user 内部创建独立页目录），
     * 后续映射全部针对其目录 */
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
    vga_write("[Shell] Code mapped at ");
    vga_write_hex(code_virt);
    vga_write("\n");

    /* 先一次性分配所有额外物理页（⚠️ 修复：失败时统一释放，不再泄漏） */
    uint32_t extra_phys[64];
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

    /* 从 FAT 加载 SHELL.BIN——⚠️ 不能直接用物理地址连续写：free_list
     * 乱序分配导致代码物理页不连续，连续写会穿到其他页（实测覆盖了
     * idle 的 PCB）。正确做法：在内核目录临时映射出连续虚拟地址，
     * 加载后解除。 */
    uint32_t load_virt = 0x20000000;   /* 内核目录未用区域 */
    for (uint32_t pi = 0; pi < code_pages; pi++) {
        uint32_t phys = (pi == 0) ? code_phys : extra_phys[pi - 1];
        vmm_map_page(vmm_get_current_directory(), load_virt + pi * 4096, phys, PAGE_WRITE);
    }

    uint32_t loaded = fat_load_file(&entry, (void*)load_virt, entry.file_size);
    for (uint32_t pi = 0; pi < code_pages; pi++) {
        vmm_unmap_page(vmm_get_current_directory(), load_virt + pi * 4096);
    }
    if (loaded != entry.file_size) {
        vga_write("[Shell] Load failed!\n");
        while(1) __asm__ volatile("hlt");
    }
    vga_write("[Shell] Loaded ");
    vga_write_hex(loaded);
    vga_write(" bytes. Executing...\n");

    /* ========== 6. 分配并映射用户栈 ========== */
    /* ⚠️ 修复：用户栈映射 2 页。main 的 x86 序言
     * （lea 4(%esp),%ecx; and $-16,%esp; push -4(%ecx)）会读栈顶
     * 之上的 4 字节——1 页栈（栈顶=0x10006000 恰好越界）时触发 #PF。 */
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
    vga_write("[Shell] Stack phys=");
    vga_write_hex(stack_phys);
    vga_write("\n");

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
    vga_write("[Shell] Stack mapped at ");
    vga_write_hex(stack_virt);
    vga_write(" (+1 guard page)\n");

    /* 登记用户页供退出时回收（代码页 + 用户栈页） */
    shell_task->user_virt_count = 0;
    for (uint32_t pi = 0; pi < code_pages && shell_task->user_virt_count < 65; pi++) {
        shell_task->user_virt_pages[shell_task->user_virt_count++] = code_virt + pi * 4096;
    }
    if (shell_task->user_virt_count < 65) {
        shell_task->user_virt_pages[shell_task->user_virt_count++] = stack_virt;
    }
    if (shell_task->user_virt_count < 65) {
        shell_task->user_virt_pages[shell_task->user_virt_count++] = stack_virt + 0x1000;
    }
    vga_write("[Shell] User task created (PID=");
    vga_write_hex(shell_task->pid);
    vga_write(").\n");

    /* ⚠️ 修复：让调度器知道真正在运行的是 shell。
     * 之前 current_task 一直是 idle——裸 iret 进用户态后，
     * int 0x80/IRQ 入口的 mov [current_task], esp 会把 shell 的
     * 中断帧写进 idle 的 PCB（污染 idle->esp）。
     * 一旦调度器切到 idle 就会从垃圾栈 popa → 崩溃。
     * 把 idle 留在 RUNNING 态即可：round-robin 只选 READY 任务，
     * idle 永远不会被选中（它也没有可执行代码）。 */
    extern task_t* current_task;
    shell_task->state = TASK_RUNNING;
    current_task = shell_task;

    /* ========== 8. 手动切换到用户任务 ==========
     *
     * 通过 iret 从 ring 0 切换到 ring 3。关键步骤：
     *
     *   a) 先设置 TSS.esp0 → 否则用户态触发 int 0x80 时 CPU
     *      用 esp0=0 压栈 → 页错误 → 三重重启（黑屏）
     *   b) 构造 iret 帧：SS=0x23, ESP=user_stack_top,
     *      EFLAGS=0x200(IF=1), CS=0x1B, EIP=code_virt
     *   c) 执行 iret → CPU 弹出帧，切换到 ring 3
     *
     * ⚠️ 之前 hardcode 了 EIP=0x01000000 和 ESP=0x01002000，
     * 但 code_virt 是动态选取的！现在用约束传递正确值。 */

    /* (a) 设置 TSS.esp0——Shell 触发 int 0x80 时的内核栈。
     *
     * 必须用任务自己的内核栈（task_create_user 里已分配），
     * 绝不能指向用户栈顶——否则内核中断帧会直接压在用户栈上，
     * 踩坏用户栈帧导致崩溃（实测：一按键就跳飞）。 */
    task_set_kernel_stack(shell_task->kernel_esp0);

    /* ⚠️ 架构升级：切换 CR3 到 shell 的独立页目录（用户代码/栈映射在
     * 该目录；内核目录里没有 256MB 虚拟区映射，不切会立即 #PF）。 */
    vmm_switch_directory(shell_task->page_directory);

    vga_write("[Shell] Executing iret... code_virt=");
    vga_write_hex(code_virt);
    vga_write("\n");

    /* 最终方案：直接内联 IRET，不经过任何函数调用 */
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

/**
 * kmain - 内核入口（GRUB 跳转的目标）
 * @magic:     Multiboot 魔数
 * @info_addr: Multiboot 信息结构体地址
 *
 * start.asm 中的 _start 调用 kmain，这里先执行 kinit 初始化
 * 所有子系统，然后根据文件系统是否就绪决定是否尝试加载 Shell。
 *
 * 如果 FAT 文件系统可用，自动进入 Shell 加载流程；否则
 * 进入死循环——没有文件系统就什么都加载不了。 */
void kmain(uint32_t magic, uint32_t info_addr) {
    if(kinit(magic, info_addr)){
        vga_write("[Auto] Attempting to load SHELL.BIN...\n");
	load_and_run_shell();
    }
    else {
        vga_write("[Auto] No filesystem, skipping shell load.\n");
        while (1) __asm__ volatile ("hlt");   /* 修复：空闲时 HLT，别让 QEMU 宿主机 CPU 100% */
    }
    /* 两个分支都不会返回（load_and_run_shell 内部已 iret 进用户态），
     * 此处不再需要 yield()。 */
}
