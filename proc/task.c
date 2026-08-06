/*
 * task.c - 协作式多任务：任务创建/回收、GDT/TSS 初始化、轮转调度
 *
 * 段选择子约定：0x08/0x10 内核代码/数据段，0x1B/0x23 用户代码/数据段
 * （RPL=3），0x28 = TSS。用户态中断时 CPU 从 TSS 取 ss0/esp0 切内核栈。
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

/* GDT 描述符（8 字节），base/limit 分段存放，access/granularity 是编码字节 */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;        /* 权限字节（P/DPL/DT/type） */
    uint8_t  granularity;   /* 粒度字节（G/D/B/L/AVL/limit_high） */
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* GDTR 加载结构（lgdt 用）：limit = 字节数 - 1 */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

/* TSS：ring3→ring0 中断时 CPU 自动取 ss0/esp0 切到内核栈。
 * 其余字段是硬件任务切换用的，本内核不用 */
typedef struct {
    uint16_t link;          /* 前一个 TSS 链接（硬件切换用） */
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
    uint32_t cr3;           /* 页目录物理地址（硬件切换用） */
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

/* 设置 GDT 第 num 个条目 */
static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access      = access;
}

/* 初始化 GDT/TSS。access: 0x9A/0x92 内核代码/数据段，0xFA/0xF2 用户
 * 代码/数据段，0x89 = TSS；gran: 0xCF = 4KB 粒度 32 位段，TSS 用 0x40 */
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

/* 释放任务资源（用户页/内核栈/PCB）。调用前须确保 task 已摘链 */
static void free_task(task_t* task) {
    if (!task) return;
    /* ⚠️ 架构升级：用户页映射在任务自己的页目录里，反查/解映射必须用
     * task->page_directory（当前 CR3 可能是其他任务的目录）。 */
    uint32_t* dir = task->page_directory ? task->page_directory : vmm_get_current_directory();
    for (uint32_t i = 0; i < task->user_virt_count && i < 65; i++) {
        uint32_t virt = task->user_virt_pages[i];
        uint32_t phys = vmm_get_phys_addr(dir, virt);
        if (phys) {
            vmm_unmap_page(dir, virt);
            pmm_free_page((void*)phys);
        }
    }
    if (task->kernel_esp0) {
        /* 内核栈页单独分配，栈顶往前一页就是栈页基址 */
        void* stack_page = (void*)(task->kernel_esp0 - PAGE_SIZE);
        pmm_free_page(stack_page);
    }
    /* ⚠️ 架构升级：销毁用户任务独立的页目录（私有页表 + 目录页） */
    if (task->page_directory && task->page_directory != vmm_get_current_directory()) {
        vmm_destroy_directory(task->page_directory);
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

/* 空闲任务：没其他任务可跑时调度器切到这。HLT 停机。
 * 不开中断：键盘/鼠标是轮询驱动，且开中断会被 IRQ 入口把 esp
 * 写进 idle->esp，污染切换帧 */
static void idle_loop(void) {
    while (1) __asm__ volatile ("hlt");
}

/* 初始化调度子系统：GDT/TSS + 创建 idle 任务。
 * ⚠️ 修复：idle 以前只有空 PCB（esp=0/eip=0），真切到它时 switch_to
 * 从地址 0 弹栈直接崩；现在用 task_create 给它完整内核栈 + idle_loop */
void task_init(void) {
    vga_write("[TASK] Initializing task manager...\n");
    init_gdt_tss();

    /* task_create 顺便把 switch_to 需要的初始上下文布好了 */
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

/* 创建内核任务：分配 PCB + 内核栈，栈上布好 [popa 帧][EBP=0][entry]，
 * 首次 switch_to 的 ret 直接跳 entry。入口函数不应返回——
 * 应最后调 task_exit() 或死循环 */
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

    /* 栈上从顶到底：popa 帧（EDI..EAX）→ EBP=0 → entry。
     * 历史教训：旧实现压栈顺序写反（镜像布局），popa 时寄存器全错位、
     * ret 越界 → 首次切换就崩 */
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

/* 创建用户任务：分配 PCB + 独立内核栈，在栈上布 [pusha 帧][iret 帧]，
 * 首次切换经 switch_to_user 手动 pop + iret 进 ring3。
 * 用户代码/栈页由调用方映射，需带 PAGE_USER 标志 */
task_t* task_create_user(const char* name, void* entry, void* stack) {
    if (!entry || !stack) return NULL;

    /* 分配 PCB */
    task_t* task = (task_t*)pmm_alloc_page();
    if (!task) return NULL;
    zero_memory(task, PAGE_SIZE);

    /* 独立内核栈页。
     * ⚠️ 历史教训：曾经把 esp0 指向用户栈顶，int 0x80/IRQ 的中断帧
     * 直接压到用户栈页上——用户栈一深（如 shell main 里 128B 的 line
     * 数组）就被踩掉返回地址，跳飞崩溃。用户任务必须有独立内核栈 */
    uint32_t* kstack = (uint32_t*)pmm_alloc_page();
    if (!kstack) {
        pmm_free_page(task);
        return NULL;
    }

    /* ⚠️ 架构升级（地址空间隔离）：每个用户任务创建独立页目录。
     * 目录复制内核全部页表（共享，无 USER 位），用户代码/栈
     * 由 load_and_run_shell 映射到该目录的私有区域。 */
    task->page_directory = vmm_create_directory();
    if (!task->page_directory) {
        pmm_free_page((void*)kstack);
        pmm_free_page(task);
        return NULL;
    }

    /* 在内核栈顶布置上下文帧 */
    uint32_t* frame = (uint32_t*)((uint32_t)kstack + PAGE_SIZE);

    /* ⚠️ 帧布局（从低到高）：[pusha 帧 32B][iret 帧 20B]，task->esp 指向
     * pusha 帧顶（EDI 槽）。恢复时 switch_to_user 手动 pop 寄存器再 iret
     * ——裸 iret 会丢通用寄存器。iret 帧按弹出顺序（EIP,CS,EFLAGS,ESP,SS）
     * 反着压 */
    *--frame = 0x23;                          /* SS  = 用户数据段, ring3 */
    *--frame = (uint32_t)stack + PAGE_SIZE;   /* ESP = 用户栈顶 */
    *--frame = 0x200;                         /* EFLAGS: IF=1 */
    *--frame = 0x1B;                          /* CS  = 用户代码段, ring3 */
    *--frame = (uint32_t)entry;               /* EIP = 用户代码入口 */
    /* pusha 帧：压栈顺序与 popa 相反（EAX 先压 = 高地址） */
    *--frame = 0;                             /* EAX */
    *--frame = 0;                             /* ECX */
    *--frame = 0;                             /* EDX */
    *--frame = 0;                             /* EBX */
    *--frame = 0;                             /* ESP（popa 跳过，值无意义） */
    *--frame = 0;                             /* EBP */
    *--frame = 0;                             /* ESI */
    *--frame = 0;                             /* EDI（最低地址 = task->esp） */

    task->pid = next_pid++;
    task->state = TASK_READY;
    task->esp = (uint32_t)frame;              /* 指向 pusha 帧顶，见上布局 */
    task->eip = (uint32_t)entry;
    task->kernel_esp0 = (uint32_t)kstack + PAGE_SIZE;  /* 内核栈顶（用户态中断用） */
    task->is_user = true;  /* 用户态任务 */
    /* page_directory 已在上面创建（独立目录） */

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

/* 设置 TSS.esp0（用户态中断时的内核栈顶）。
 * ⚠️ 不经调度器直接 iret 进用户态前必须调：否则 int 0x80/IRQ 时 CPU
 * 用 esp0=0 压栈 → 页错误 → 三重故障黑屏重启。调度路径由 schedule()
 * 自动设置，无需调用 */
void task_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

/* 主动让出 CPU——协作式调度下任务不主动让就没得切换 */
void yield(void) {
    schedule();
}

/* 轮转调度：终止任务走回收分支，否则找下一个 READY 任务；
 * 切栈前更新 TSS.esp0，按 is_user 选 switch_to / switch_to_user */
void schedule(void) {
    if (!current_task) return;

    /* ⚠️ 调度关键段关中断：pusha 不保存 EFLAGS，内核任务经 switch_to
     * 恢复时 IF 不受控，IF=1 下被 IRQ 打断会并发改链表 → 崩。
     * 退出用 popfl 还原原 IF；IRQ 上下文（IF=0）里不能贸然 sti */
    uint32_t saved_flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(saved_flags));

    /* 终止任务分支 */
    if (current_task->state == TASK_TERMINATED) {
        task_t* prev = NULL;
        task_t* t = task_list;
        while (t && t != current_task) {
            prev = t;
            t = t->next;
        }

        if (t == current_task) {
            if (prev) prev->next = current_task->next;
            else task_list = current_task->next;
        }

        task_t* next_task = current_task->next;
        /* ⚠️ 正跑在被终止任务自己的内核栈上，当场 free 是 use-after-free，
         * 先挂僵尸链表（详见 zombie_list 注释） */
        free_task_later(current_task);

        if (!next_task) next_task = task_list;
        if (!next_task) {
            /* 没有任务了——死循环 HLT（系统挂起） */
            while (1) __asm__ volatile ("hlt");
        }

        /* ⚠️ 修复：以前直接切 current_task->next 不查就绪态——shell 退出
         * 时会切到 RUNNING 的 idle，把 READY 的 demo 饿死。先找就绪任务 */
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

        /* 同正常分支：切换 CR3 到目标任务页目录 */
        if (current_task->page_directory &&
            current_task->page_directory != vmm_get_current_directory()) {
            vmm_switch_directory(current_task->page_directory);
        }

        /* ⚠️ 修复：以前用 == idle_task 判类型，demo 这类内核任务不是 idle，
         * 被错走 switch_to_user，iret 从 pusha 帧弹垃圾 → #GP。改判 is_user */
        if (!current_task->is_user) {
            switch_to(NULL, current_task);        /* 内核任务（含 idle） */
        } else {
            switch_to_user(NULL, current_task);   /* 用户任务 */
        }
        goto out;
    }

    /* 正常轮转分支 */
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

    /* 当前栈是活的，先回收僵尸任务内存（安全点） */
    reap_zombies();

    task_t* prev = current_task;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;

    if (next->kernel_esp0) {
        tss.esp0 = next->kernel_esp0;   /* 用户态中断切回的内核栈 */
    }

    current_task = next;

    /* ⚠️ 架构升级（地址空间隔离）：切换 CR3 到目标任务页目录。
     * 内核任务/idle 的 page_directory 就是内核目录，切换无效果；
     * 用户任务切到自己的独立目录（复制了内核页表，切后内核代码
     * 和内核栈仍可访问，安全）。 */
    if (next->page_directory && next->page_directory != vmm_get_current_directory()) {
        vmm_switch_directory(next->page_directory);
    }

    /* 按 is_user 选切换方式（kernel_esp0 内核任务也有，不能当判据）。
     * prev 是用户任务时必须传 NULL：它的 esp 由中断入口存进 PCB 了，
     * 再被 pusha 覆盖会丢掉 iret 帧位置 */
    if (next == idle_task) {
        switch_to(!prev->is_user ? prev : NULL, next);  /* 内核任务切换 */
    } else {
        if (next->is_user) {
            switch_to_user(!prev->is_user ? prev : NULL, next);  /* 用户任务切换 */
        } else {
            switch_to(!prev->is_user ? prev : NULL, next);  /* 内核任务切换：
                 prev 是用户任务时传 NULL——pusha 会把深层栈指针写进
                 shell->esp，切回时 iret 弹垃圾帧 → #GP（实测踩过） */
        }
    }
out:
    __asm__ volatile ("pushl %0; popfl" : : "r"(saved_flags));
}

task_t* task_current(void) {
    return current_task;
}

/* 终止当前任务：标记 TERMINATED 后循环 schedule()，等调度器回收 */
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

/* 调试用：打印全部任务，当前任务标 <- CURRENT */
void task_dump_all(void) {
    task_t* t = task_list;
    vga_write("[TASK] Task list:\n");
    vga_write("[TASK] task_list=");
    vga_write_hex((uint32_t)(uintptr_t)task_list);
    vga_write(" current=");
    vga_write_hex((uint32_t)(uintptr_t)current_task);
    vga_write("\n");
    while (t) {
        vga_write("  PID=");
        vga_write_hex(t->pid);
        vga_write(" @");
        vga_write_hex((uint32_t)(uintptr_t)t);
        vga_write(" next=");
        vga_write_hex((uint32_t)(uintptr_t)t->next);
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
