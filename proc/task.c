/**
 * =========================================================================
 * task.c - 进程/任务管理器（协作式多任务）
 *
 * HaoOS 实现了简单的协作式（非抢占式）多任务调度。任务通过主动调用
 * yield() 让出 CPU，调度器按轮转（Round-Robin）方式选择下一个就绪任务。
 *
 * GDT 布局：
 *   索引 0: 空描述符（必须，x86 要求 GDT[0] = 0）
 *   索引 1: 内核代码段 base=0 limit=4GB access=0x9A gran=0xCF
 *   索引 2: 内核数据段 base=0 limit=4GB access=0x92 gran=0xCF
 *   索引 3: 用户代码段 base=0 limit=4GB access=0xFA gran=0xCF
 *   索引 4: 用户数据段 base=0 limit=4GB access=0xF2 gran=0xCF
 *   索引 5: TSS（任务状态段）
 *
 * 选择子对应关系：
 *   0x08 = GDT[1] 内核代码段 (ring 0)
 *   0x10 = GDT[2] 内核数据段 (ring 0)
 *   0x1B = GDT[3] 用户代码段 (ring 3)——注意 RPL = 3
 *   0x23 = GDT[4] 用户数据段 (ring 3)——注意 RPL = 3
 *   0x28 = GDT[5] TSS
 *
 * TSS（任务状态段）的作用：
 *   当用户态任务触发中断（如 int 0x80）时，CPU 会自动从 TSS 加载
 *   SS0 和 ESP0，切换到内核栈，然后压入用户态上下文。因此每个
 *   任务需要有自己的内核栈，调度时需要更新 TSS.esp0。
 * =========================================================================
 */

#include "../include/proc/task.h"
#include "../include/mm/pmm.h"
#include "../include/mm/vmm.h"
#include "../include/driver/vga.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 内存清零辅助函数 */
static inline void zero_memory(void* ptr, size_t size) {
    unsigned char* p = (unsigned char*)ptr;
    while (size--) *p++ = 0;
}

/* ===================== GDT 和 TSS 结构定义 ===================== */

/**
 * gdt_entry_t - GDT 描述符（8 字节）
 * 描述一个内存段的基础、限制和访问权限。
 * 位布局（从高到低）：
 *   高 4 字节: base_high(8) | granularity(4+4) | access(8) | base_mid(8)
 *   低 4 字节: base_low(16) | limit_low(16)
 */
typedef struct {
    uint16_t limit_low;     /* 段界限低 16 位 */
    uint16_t base_low;      /* 基地址低 16 位 */
    uint8_t  base_mid;      /* 基地址中 8 位 */
    uint8_t  access;        /* 访问权限（P,DPL,DT,type） */
    uint8_t  granularity;   /* 粒度（G,D/B,L,AVL,limit_high） */
    uint8_t  base_high;     /* 基地址高 8 位 */
} __attribute__((packed)) gdt_entry_t;

/**
 * gdt_ptr_t - GDTR 加载结构
 * 用于 lgdt 指令。limit = GDT 字节数 - 1，base = GDT 线性地址。
 */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

/**
 * tss_entry_t - 任务状态段（TSS, Task State Segment）
 *
 * 当 CPU 从用户态（ring 3）切换到内核态（ring 0）时，TSS 提供
 * 内核栈的 SS0 和 ESP0。这在 int 0x80 系统调用和 IRQ 中断时使用。
 *
 * 字段说明：
 *   ss0/esp0: ring 0（内核）的栈段和栈指针
 *   cr3:      任务自己的页目录物理地址（如果支持独立地址空间）
 *   其他字段用于硬件任务切换（HaoOS 不使用硬件任务切换）
 */
typedef struct {
    uint16_t link;          /* 前一个 TSS 链接（任务切换用） */
    uint16_t reserved0;
    uint32_t esp0;          /* ring 0 栈指针（内核栈） */
    uint16_t ss0;           /* ring 0 栈段 */
    uint16_t reserved1;
    uint32_t esp1;
    uint16_t ss1;
    uint16_t reserved2;
    uint32_t esp2;
    uint16_t ss2;
    uint16_t reserved3;
    uint32_t cr3;           /* 页目录物理地址 */
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint16_t es, reserved4;
    uint16_t cs, reserved5;
    uint16_t ss, reserved6;
    uint16_t ds, reserved7;
    uint16_t fs, reserved8;
    uint16_t gs, reserved9;
    uint16_t ldt_selector;
    uint16_t reserved10;
    uint16_t t;             /* 调试陷阱标志 */
    uint16_t iomap_base;    /* I/O 位图基址偏移 */
} __attribute__((packed)) tss_entry_t;

/* 全局变量 */
static gdt_entry_t gdt[6];      /* GDT 表：6 个条目 */
static gdt_ptr_t gdt_ptr;       /* GDTR 值 */
static tss_entry_t tss;         /* TSS */
static task_t* task_list = NULL;     /* 任务链表头 */
/* current_task 非 static：isr_asm.asm 的中断入口需要保存
 * current_task->esp（用户任务的中断帧位置），供调度器切回用 */
task_t* current_task = NULL;         /* 当前运行的任务 */
static task_t* idle_task = NULL;     /* 空闲任务 */
static uint32_t next_pid = 1;        /* 下一个 PID */

/* 外部汇编函数 */
extern void switch_to(task_t* prev, task_t* next);      /* 内核任务切换 */
extern void switch_to_user(task_t* prev, task_t* next); /* 切换到用户任务 */

/* ===================== GDT 和 TSS 初始化 ===================== */

/**
 * gdt_set_gate - 设置 GDT 的一个条目
 * @num:    GDT 索引（0~5）
 * @base:   段基址
 * @limit:  段界限
 * @access: 访问权限字节
 * @gran:   粒度字节
 */
static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access      = access;
}

/**
 * init_gdt_tss - 初始化 GDT 和 TSS
 *
 * GDT 条目属性说明：
 *   access=0x9A → 代码段，P=1, DPL=0, 可执行可读
 *   access=0x92 → 数据段，P=1, DPL=0, 可读写
 *   access=0xFA → 代码段，P=1, DPL=3, 可执行可读
 *   access=0xF2 → 数据段，P=1, DPL=3, 可读写
 *   access=0x89 → TSS，P=1, DPL=0, 类型=9（32 位可用 TSS）
 *   gran=0xCF   → G=1（4KB 粒度）, D/B=1（32 位）, limit_high=0xF
 *   gran=0x40   → G=0（字节粒度）, D/B=0（16 位 TSS）
 */
static void init_gdt_tss(void) {
    /* 清零 TSS */
    zero_memory(&tss, sizeof(tss_entry_t));
    tss.ss0 = 0x10;              /* ring 0 栈段 = 内核数据段 */
    tss.esp0 = 0;                /* 稍后由调度器设置 */

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint32_t)&gdt;

    /* GDT[0]: 空描述符（x86 必须） */
    gdt_set_gate(0, 0, 0, 0, 0);
    /* GDT[1]: 内核代码段（base=0, limit=4GB, ring 0） */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    /* GDT[2]: 内核数据段（base=0, limit=4GB, ring 0） */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    /* GDT[3]: 用户代码段（base=0, limit=4GB, ring 3） */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    /* GDT[4]: 用户数据段（base=0, limit=4GB, ring 3） */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    /* GDT[5]: TSS（base=TSS 地址, limit=sizeof(TSS)） */
    gdt_set_gate(5, (uint32_t)&tss, sizeof(tss_entry_t) - 1, 0x89, 0x40);

    /* 加载 GDTR */
    __asm__ volatile ("lgdt (%0)" : : "r"(&gdt_ptr));

    /* 用新的 GDT 段选择子重新加载段寄存器 */
    __asm__ volatile (
        "mov $0x10, %%ax\n"    /* 内核数据段 */
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "ljmp $0x08, $1f\n"    /* 远跳转刷新 CS（内核代码段） */
        "1:\n"
        : : : "memory", "eax"
    );

    /* 加载任务寄存器（TR）指向 GDT[5] = TSS */
    __asm__ volatile ("ltr %%ax" : : "a" (0x28));

    vga_write("[TASK] GDT and TSS initialized.\n");
}

/* ===================== 任务资源管理 ===================== */

/**
 * free_task - 释放任务占用的资源
 * @task: 要释放的任务指针
 *
 * 释放内核栈页和 PCB 页。注意：不会从链表移除任务，
 * 调用者需确保 task 已不在链表中。
 */
static void free_task(task_t* task) {
    if (!task) return;
    /* ⚠️ 修复：回收用户任务映射的物理页（代码/用户栈）。
     * 逐个 unmap + 释放——旧实现退出后这些页永久泄漏。 */
    for (uint32_t i = 0; i < task->user_virt_count && i < 65; i++) {
        uint32_t virt = task->user_virt_pages[i];
        uint32_t phys = vmm_get_phys_addr(vmm_get_current_directory(), virt);
        if (phys) {
            vmm_unmap_page(vmm_get_current_directory(), virt);
            pmm_free_page((void*)phys);
        }
    }
    if (task->kernel_esp0) {
        /* 内核栈在 PCB 之后单独分配，需要释放 */
        void* stack_page = (void*)(task->kernel_esp0 - PAGE_SIZE);
        pmm_free_page(stack_page);
    }
    pmm_free_page(task);  /* 释放 PCB 页 */
}

/* ⚠️ 僵尸回收：被终止任务的 PCB/内核栈不能当场释放——
 * schedule() 的终止分支正运行在被终止任务的内核栈上，
 * 切到下一个任务前释放它会 use-after-free（旧实现踩的坑）。
 * 做法：先挂入僵尸链表，等下次正常轮转切换前再统一回收。 */
static task_t* zombie_list = NULL;

/** 把已摘链的终止任务挂入僵尸链表（复用 next 字段） */
static void free_task_later(task_t* task) {
    task->next = zombie_list;
    zombie_list = task;
}

/** 回收僵尸任务的内存（仅在当前任务栈安全时调用：正常轮转分支） */
static void reap_zombies(void) {
    while (zombie_list) {
        task_t* z = zombie_list;
        zombie_list = z->next;
        free_task(z);
    }
}

/* ===================== 初始化 ===================== */

/**
 * idle_loop - 空闲任务体
 *
 * 没有其他就绪任务时调度器会切到这里。
 * HLT 停机等待（不开中断——键盘/鼠标是轮询驱动，不依赖 IRQ；
 * 且开中断会被 IRQ 入口把当前 esp 写进 idle->esp，污染切换帧）。
 */
static void idle_loop(void) {
    while (1) __asm__ volatile ("hlt");
}

/**
 * task_init - 初始化进程管理子系统
 *
 * 创建空闲任务（idle task）——当没有其他任务可运行时，
 * 调度器选择 idle 任务。
 *
 * ⚠️ 修复：idle 之前只有 PCB（esp=0/eip=0，无栈无代码），
 * 一旦调度器真切到 idle（比如所有任务都终止），switch_to 会
 * 从地址 0 弹栈 → 直接崩溃。现在用 task_create 给它完整的内核
 * 栈 + 入口（idle_loop）。
 */
void task_init(void) {
    vga_write("[TASK] Initializing task manager...\n");
    init_gdt_tss();

    /* 用 task_create 创建 idle：自动分配 PCB + 内核栈，
     * 布置好 switch_to 可恢复的初始上下文（entry=idle_loop） */
    task_t* idle = task_create("idle", idle_loop);
    if (!idle) {
        vga_write("[TASK] ERROR: Failed to allocate idle task!\n");
        return;
    }
    idle->state = TASK_RUNNING;
    idle->kernel_esp0 = 0;   /* idle 无用户态，不需要中断栈（语义清晰） */
    idle->next = NULL;       /* idle 固定在链表尾 */

    task_list = idle;
    current_task = idle;
    idle_task = idle;

    vga_write("[TASK] Idle task created (PID=1).\n");
}

/* ===================== 创建内核任务 ===================== */

/**
 * task_create - 创建一个内核任务（内核线程）
 * @name:  任务名称（用于调试输出）
 * @entry: 入口函数指针
 *
 * 分配 PCB 页和内核栈页。在栈上布置初始上下文：
 *   - 栈顶压入 entry（入口地址），这样 switch_to 后的 ret
 *     会直接跳转到任务入口
 *   - 其他位置压入 0（作为 pusha 的对应初始值）
 *
 * 注意：内核任务的入口函数不应该返回。如果返回，EBP=0 时的
 * 行为未定义。入口函数应最后调用 task_exit() 或进入死循环。
 */
task_t* task_create(const char* name, void (*entry)(void)) {
    if (!entry) return NULL;

    /* 分配 PCB */
    task_t* task = (task_t*)pmm_alloc_page();
    if (!task) return NULL;
    zero_memory(task, PAGE_SIZE);

    /* 分配内核栈 */
    uint32_t* stack = (uint32_t*)pmm_alloc_page();
    if (!stack) {
        pmm_free_page(task);
        return NULL;
    }

    /* --- 在栈上布置初始上下文 --- */
    /* 栈布局（从顶到底，与 switch_to 的 popa 顺序严格对应）：
     *   EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX （popa 依次弹出）
     *   再下面：初始 EBP=0（pop ebp 弹出）
     *   最底下：entry（ret 弹出 → 跳转到任务入口）
     *
     * 历史教训：旧实现把压栈顺序写反了（镜像布局），
     * popa 时寄存器全部错位，EBX 会拿到垃圾值、EBP 会拿到
     * entry、ret 会从栈底越界弹出 → 第一次切换就崩。 */
    uint32_t* stack_ptr = (uint32_t*)((uint32_t)stack + PAGE_SIZE);

    *(--stack_ptr) = (uint32_t)entry;   /* ret 弹出 → 跳到入口 */
    *(--stack_ptr) = 0;                  /* 初始 EBP（pop ebp 弹出） */
    *(--stack_ptr) = 0;                  /* EAX */
    *(--stack_ptr) = 0;                  /* ECX */
    *(--stack_ptr) = 0;                  /* EDX */
    *(--stack_ptr) = 0;                  /* EBX */
    *(--stack_ptr) = 0;                  /* ESP（popa 丢弃） */
    *(--stack_ptr) = 0;                  /* EBP */
    *(--stack_ptr) = 0;                  /* ESI */
    *(--stack_ptr) = 0;                  /* EDI（栈顶，task->esp 指向） */

    task->esp = (uint32_t)stack_ptr;
    task->eip = (uint32_t)entry;
    task->pid = next_pid++;
    task->state = TASK_READY;
    task->kernel_esp0 = (uint32_t)stack + PAGE_SIZE;  /* 内核栈顶 */
    task->is_user = false;  /* 内核任务 */
    task->page_directory = vmm_get_current_directory();

    /* 复制任务名 */
    int i;
    for (i = 0; i < 31 && name && name[i]; i++)
        task->name[i] = name[i];
    task->name[i] = '\0';

    /* 插入链表头 */
    task->next = task_list;
    task_list = task;

    vga_write("[TASK] Created task '");
    vga_write(name);
    vga_write("' (PID=");
    vga_write_hex(task->pid);
    vga_write(")\n");

    return task;
}

/* ===================== 创建用户任务 ===================== */

/**
 * task_create_user - 创建一个用户态任务
 * @name:  任务名称
 * @entry: 入口虚拟地址（用户代码的线性地址）
 * @stack: 用户栈虚拟地址（不需提前分配页）
 *
 * 在任务的内核栈上布置 iret 返回帧，使得首次切换到该任务时
 * 通过 iret 进入用户态（ring 3）。
 *
 * 栈帧布局（从高到低）：
 *   SS     = 0x23（用户数据段选择子，ring 3）
 *   ESP    = stack_top（用户栈顶）
 *   EFLAGS = 0x200（IF=1 开启中断）
 *   CS     = 0x1B（用户代码段选择子，ring 3）
 *   EIP    = entry（用户代码入口）
 *
 * 注意：用户任务需要正确的页表映射（PAGE_USER 标志），
 * 否则在执行时会触发页错误。
 */
task_t* task_create_user(const char* name, void* entry, void* stack) {
    if (!entry || !stack) return NULL;

    /* 分配 PCB */
    task_t* task = (task_t*)pmm_alloc_page();
    if (!task) return NULL;
    zero_memory(task, PAGE_SIZE);

    /* 分配独立的内核栈页。
     *
     * ⚠️ 历史教训：这里曾经把 esp0 指向用户栈顶，导致内核在
     * int 0x80 / IRQ 时把中断帧直接压在用户栈页上。用户程序一旦
     * 栈用得深（比如 shell 的 main 里有 128 字节的 line 数组），
     * 内核帧就会踩掉用户栈帧 → 返回地址损坏 → 跳飞崩溃。
     * 用户态任务必须拥有自己的内核栈。 */
    uint32_t* kstack = (uint32_t*)pmm_alloc_page();
    if (!kstack) {
        pmm_free_page(task);
        return NULL;
    }

    /* 在任务自己的内核栈上布置上下文帧 */
    uint32_t* frame = (uint32_t*)((uint32_t)kstack + PAGE_SIZE);

    /* ⚠️ 布局（从低到高）：[pusha 帧 32B][iret 帧 20B]，task->esp 指向
     * pusha 帧顶部（EDI 槽）。恢复时 switch_to_user 手动 pop 寄存器
     * 再 iret——通用寄存器才能完整恢复（裸 iret 会丢寄存器）。
     * iret 帧按 iret 弹出顺序（EIP, CS, EFLAGS, ESP, SS）反着压 */
    *--frame = 0x23;                          /* SS  = 用户数据段, ring 3 */
    *--frame = (uint32_t)stack + PAGE_SIZE;   /* ESP = 用户栈顶 */
    *--frame = 0x200;                         /* EFLAGS: IF=1（开中断） */
    *--frame = 0x1B;                          /* CS  = 用户代码段, ring 3 */
    *--frame = (uint32_t)entry;               /* EIP = 用户代码入口 */
    /* pusha 帧：popa 顺序 EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX，
     * 压栈时反着压（EAX 先压 = 最高地址） */
    *--frame = 0;                             /* EAX */
    *--frame = 0;                             /* ECX */
    *--frame = 0;                             /* EDX */
    *--frame = 0;                             /* EBX */
    *--frame = 0;                             /* ESP（pop 时跳过，值无意义） */
    *--frame = 0;                             /* EBP */
    *--frame = 0;                             /* ESI */
    *--frame = 0;                             /* EDI（最低地址 = task->esp） */

    task->pid = next_pid++;
    task->state = TASK_READY;
    task->esp = (uint32_t)frame;              /* 栈指针指向内核栈上的 iret 帧 */
    task->eip = (uint32_t)entry;
    task->kernel_esp0 = (uint32_t)kstack + PAGE_SIZE;  /* 内核栈顶（用户态中断用） */
    task->is_user = true;  /* 用户态任务 */
    task->page_directory = vmm_get_current_directory();

    int i;
    for (i = 0; i < 31 && name && name[i]; i++)
        task->name[i] = name[i];
    task->name[i] = '\0';

    /* 插入链表头 */
    task->next = task_list;
    task_list = task;

    vga_write("[TASK] User stack frame: SS=0x23 ESP=0x");
    vga_write_hex((uint32_t)stack + PAGE_SIZE);
    vga_write(" EFLAGS=0x200 CS=0x1B EIP=0x");
    vga_write_hex((uint32_t)entry);
    vga_write("\n");

    return task;
}

/* ===================== TSS 内核栈设置 ===================== */

/**
 * task_set_kernel_stack - 设置 TSS.esp0（用户态中断时的内核栈）
 * @esp0: 内核栈顶指针
 *
 * 当直接通过 iret 切换到用户态（不经过调度器）时，必须手动设置
 * TSS.esp0！否则用户态触发 int 0x80 时，CPU 加载 TSS.esp0=0
 * 作为内核栈指针 → 向物理地址 0xFFFFFFFC 压栈 → 页错误 → 三重故障
 * （系统重启，黑屏无输出）。
 *
 * 正常经过调度器的切换流程中，schedule() 在 switch_to 之前会
 * 自动设置 TSS.esp0 为 next->kernel_esp0。
 */
void task_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

/* ===================== 调度器 ===================== */

/**
 * yield - 主动让出 CPU
 *
 * 当前任务调用 yield() 后，调度器查找下一个就绪任务并切换。
 * 这是协作式多任务的核心——任务必须主动让出 CPU，没有时间片
 * 抢占。
 */
void yield(void) {
    schedule();
}

/**
 * schedule - 调度器：选择下一个任务并切换
 *
 * 调度策略：简单的轮转（Round-Robin）
 *   1. 如果当前任务已终止（TASK_TERMINATED）：
 *      - 从链表移除
 *      - 释放资源
 *      - 选取下一个任务
 *   2. 否则按链表顺序查找下一个 TASK_READY 的任务
 *   3. 找到后保存当前任务状态，切换到目标任务
 *
 * 区分 idle 和其他任务的切换方式：
 *   - 切换到 idle：使用 switch_to（内核任务切换）
 *   - 切换到用户任务：使用 switch_to_user（iret/retf 方式）
 *   - 内核任务间切换：使用 switch_to（pusha/popa 方式）
 *
 * 注意：每次切换前会更新 TSS.esp0，确保用户态中断后能回到正确的内核栈。
 */
void schedule(void) {
    if (!current_task) return;

    /* ⚠️ 修复：调度关键段关闭中断。
     * pusha 不保存 EFLAGS，内核任务经 switch_to 恢复时 IF 状态不受控；
     * 若在 IF=1 下被 IRQ 打断，链表/current_task 会被并发修改 → 崩溃。
     * 退出时用 popfl 还原原 IF 状态——不能在 IRQ 上下文（IF=0 进入）里
     * 贸然 sti，否则会嵌套重入中断。 */
    uint32_t saved_flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(saved_flags));

    /* ========== 终止任务处理 ========== */
    if (current_task->state == TASK_TERMINATED) {
        task_t* prev = NULL;
        task_t* t = task_list;
        /* 在链表中查找当前任务 */
        while (t && t != current_task) {
            prev = t;
            t = t->next;
        }

        if (t == current_task) {
            if (prev) prev->next = current_task->next;
            else task_list = current_task->next;
        }

        task_t* next_task = current_task->next;
        /* ⚠️ 修复：不能当场 free_task——当前正运行在被终止任务自己的
         * 内核栈上，切到下一个任务前释放它会 use-after-free。
         * 挂入僵尸链表，由下次正常轮转统一回收。 */
        free_task_later(current_task);

        if (!next_task) next_task = task_list;
        if (!next_task) {
            /* 没有任务了——死循环 HLT（系统挂起） */
            while (1) __asm__ volatile ("hlt");
        }

        /* ⚠️ 修复：终止路径原来直接切到 current_task->next，
         * 不检查就绪状态——shell 退出时会切到 RUNNING 态的 idle，
         * 导致 READY 的 demo 饿死。这里改成优先找下一个就绪任务，
         * 找不到才退回原选择（比如只剩 idle）。 */
        {
            task_t* pick = next_task;
            int tries = 0;
            while (pick->state != TASK_READY && tries < MAX_TASKS) {
                pick = pick->next ? pick->next : task_list;
                tries++;
            }
            if (pick->state == TASK_READY) next_task = pick;
        }

        current_task = next_task;
        current_task->state = TASK_RUNNING;

        /* ⚠️ 修复：这里原来用 current_task == idle_task 判断任务类型，
         * 但 demo 这类内核任务不是 idle——被错误地走 switch_to_user，
         * iret 从 pusha 帧弹垃圾 → #GP。统一用 is_user 判断。 */
        if (!current_task->is_user) {
            switch_to(NULL, current_task);        /* 内核任务（含 idle） */
        } else {
            switch_to_user(NULL, current_task);   /* 用户任务 */
        }
        goto out;
    }

    /* ========== 正常轮转调度 ========== */
    task_t* next = current_task->next;
    if (!next) next = task_list;                   /* 链表末尾回到开头 */
    if (next == current_task) goto out;            /* 只有自己，无需切换 */

    /* 查找下一个就绪的任务，最多尝试 MAX_TASKS 次 */
    int tries = 0;
    while (next->state != TASK_READY && tries < MAX_TASKS) {
        next = next->next;
        if (!next) next = task_list;
        tries++;
        if (next == current_task) goto out;        /* 没有其他就绪任务 */
    }
    if (next->state != TASK_READY) goto out;       /* 没有就绪任务 */

    /* 找到就绪任务，切换前回收僵尸任务的内存（此时当前栈是活的，安全） */
    reap_zombies();

    /* 更新 TSS.esp0——如果目标任务有内核栈，设置它为中断入口栈 */
    task_t* prev = current_task;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;

    if (next->kernel_esp0) {
        tss.esp0 = next->kernel_esp0;              /* 更新内核栈指针 */
    }

    current_task = next;

    /* 根据任务类型选择合适的切换方式。
     * ⚠️ 用 is_user 判断（kernel_esp0 对内核任务也非零，不能用作判据）：
     *   - 切换到用户任务 → switch_to_user（从 iret 帧恢复）
     *   - 切换到内核任务 → switch_to（从 pusha 帧恢复）
     * prev 若是用户任务，其 esp 已由中断入口（isr80_handler/IRQ）
     * 保存到 PCB，不能再被 pusha 的栈指针覆盖 → 传 NULL。 */
    if (next == idle_task) {
        switch_to(!prev->is_user ? prev : NULL, next);  /* 内核任务切换 */
    } else {
        if (next->is_user) {
            switch_to_user(!prev->is_user ? prev : NULL, next);  /* 用户任务切换 */
        } else {
            switch_to(!prev->is_user ? prev : NULL, next);  /* 内核任务切换：
                 prev 是用户任务时必须传 NULL——它的上下文由中断入口保存，
                 pusha 会覆盖 iret 帧指针（实测：shell→demo 时把深层栈指针
                 写进 shell->esp，切回时 iret 弹垃圾帧 → #GP） */
        }
    }
out:
    __asm__ volatile ("pushl %0; popfl" : : "r"(saved_flags));
}

/* ===================== 辅助函数 ===================== */

task_t* task_current(void) {
    return current_task;
}

/**
 * task_exit - 终止当前任务
 *
 * 将当前任务标记为终止，然后持续调用 schedule() 等待被清理。
 * schedule() 在检测到 TASK_TERMINATED 后会移除此任务并释放资源。
 */
void task_exit(void) {
    if (!current_task) return;
    current_task->state = TASK_TERMINATED;
    vga_write("[TASK] Task PID=");
    vga_write_hex(current_task->pid);
    vga_write(" terminated.\n");
    while (1) {
        schedule();
        __asm__ volatile ("hlt");
    }
}

/**
 * task_dump_all - 调试用：打印所有任务状态
 *
 * 遍历任务链表，输出每个任务的关键信息：PID、名称、状态、
 * ESP 和 EIP，当前任务会标记 <- CURRENT。
 */
void task_dump_all(void) {
    task_t* t = task_list;
    vga_write("[TASK] Task list:\n");
    while (t) {
        vga_write("  PID=");
        vga_write_hex(t->pid);
        vga_write(" name='");
        vga_write(t->name);
        vga_write("' state=");
        switch (t->state) {
            case TASK_RUNNING:    vga_write("RUNNING"); break;
            case TASK_READY:      vga_write("READY"); break;
            case TASK_BLOCKED:    vga_write("BLOCKED"); break;
            case TASK_TERMINATED: vga_write("TERMINATED"); break;
            default:              vga_write("UNKNOWN");
        }
        vga_write(" esp=");
        vga_write_hex(t->esp);
        vga_write(" eip=");
        vga_write_hex(t->eip);
        if (t == current_task) vga_write(" <- CURRENT");
        vga_write("\n");
        t = t->next;
    }
}
