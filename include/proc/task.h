/**
 * =========================================================================
 * task.h - 进程/任务管理接口
 *
 * HaoOS 实现了一个简单的协作式多任务调度器。任务（task）也称为进程，
 * 每个任务拥有自己的栈和上下文（寄存器状态），通过主动调用 yield()
 * 或 schedule() 让出 CPU。
 *
 * 数据结构：
 *   任务控制块（PCB, task_t）保存了任务的全部状态。最关键的是 esp 和
 *   eip 字段——它们必须放在结构体开头（偏移 0 和 4），因为汇编代码
 *   switch_to.asm 中直接使用 [task+0] 和 [task+4] 访问它们。
 *
 * GDT 布局：
 *   索引 0: null 描述符
 *   索引 1: 内核代码段（CS=0x08, ring 0）
 *   索引 2: 内核数据段（DS=0x10, ring 0）
 *   索引 3: 用户代码段（CS=0x1B, ring 3）
 *   索引 4: 用户数据段（DS=0x23, ring 3）
 *   索引 5: TSS 段（TR=0x28），用于用户态任务的栈切换
 *
 * TSS（任务状态段）：
 *   用于用户态→内核态切换时，CPU 自动从 TSS 加载内核栈指针（esp0）。
 *   每个任务都有自己的内核栈，调度时更新 TSS.esp0。
 * =========================================================================
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

/* 最大任务数量（简单数组上界，防止无限循环） */
#define MAX_TASKS 32

/* ---- 任务状态枚举 ---- */
typedef enum {
    TASK_RUNNING,      /* 当前正在运行 */
    TASK_READY,        /* 就绪，等待调度 */
    TASK_BLOCKED,      /* 阻塞（等待某事件） */
    TASK_TERMINATED    /* 已终止，等待被清理 */
} task_state_t;

/**
 * task_t - 任务控制块（PCB, Process Control Block）
 *
 * ⚠️ 布局约束：esp 和 eip 必须位于偏移 0 和 4！
 *     switch_to() 的汇编代码通过 [eax+0] 和 [edx+0] 访问它们，
 *     popa/pusha 的栈布局也依赖此顺序。
 */
typedef struct task_struct {
    /* ---- 上下文切换（布局固定，与汇编约定一致）---- */
    uint32_t esp;           /* 偏移 0：栈指针（保存任务的栈顶） */
    uint32_t eip;           /* 偏移 4：指令指针（下一条要执行的指令地址） */

    /* ---- 任务属性 ---- */
    uint32_t pid;           /* 进程 ID */
    task_state_t state;     /* 当前状态 */

    /* ---- 内核栈（用于用户态中断处理） ---- */
    uint32_t kernel_esp0;   /* 用户态任务的中断会切换到内核栈，此字段存栈顶 */

    /* ---- 地址空间 ---- */
    uint32_t* page_directory;  /* 页目录指针（独立地址空间用，当前统一为内核地址空间） */

    /* ---- 链表 ---- */
    struct task_struct* next;  /* 指向下一个任务的指针（形成单向链表） */

    /* ---- 调试信息 ---- */
    char name[32];          /* 任务名称 */
} task_t;

/* ---- 函数声明 ---- */

/**
 * task_init - 初始化进程管理子系统
 *
 * 初始化 GDT 和 TSS，创建空闲任务（idle task）。
 * 空闲任务在没有其他任务可运行时执行（当前 loop）。
 */
void task_init(void);

/**
 * task_create - 创建一个内核线程
 * @name:  任务名称（调试用）
 * @entry: 入口函数指针
 * 返回值：指向新任务 PCB 的指针，失败返回 NULL
 *
 * 分配 PCB 和内核栈，在栈上布置初始上下文（压入入口地址），
 * 这样第一次 switch_to 时会通过 ret 指令跳转到入口。
 */
task_t* task_create(const char* name, void (*entry)(void));

/**
 * task_create_user - 创建一个用户态任务
 * @name:  任务名称
 * @entry: 入口虚拟地址
 * @stack: 用户栈的虚拟地址
 * 返回值：指向新任务 PCB 的指针，失败返回 NULL
 *
 * 在栈上布置中断返回帧（SS, ESP, EFLAGS, CS, EIP），
 * 通过 iret 指令首次切换到用户态。
 */
task_t* task_create_user(const char* name, void* entry, void* stack);

/**
 * task_set_kernel_stack - 设置 TSS.esp0
 * @esp0: 用户态中断时 CPU 使用的内核栈顶指针
 *
 * 直接通过 iret 切换到用户态前必须调用此函数设定内核栈。
 * 否则 int 0x80/IRQ 触发时 CPU 用 esp0=0 压栈 → 崩溃。
 */
void task_set_kernel_stack(uint32_t esp0);

/**
 * yield - 主动让出 CPU
 * 当前任务调用此函数，调度器选择下一个就绪任务切换过去。
 */
void yield(void);

/**
 * schedule - 调度器
 *
 * 简单的轮转（Round-Robin）调度：
 *   1. 如果当前任务已终止，从链表移除并释放
 *   2. 否则遍历链表找下一个 TASK_READY 的任务
 *   3. 找到后保存当前上下文，切换到目标任务
 */
void schedule(void);

/**
 * task_current - 获取当前正在运行的任务指针
 */
task_t* task_current(void);

/**
 * task_exit - 终止当前任务
 * 将当前任务标记为 TASK_TERMINATED，然后进入调度循环等待清理。
 */
void task_exit(void);

/**
 * task_dump_all - 打印所有任务状态（调试用）
 * 遍历任务链表，输出每个任务的 PID、名称、状态、ESP 和 EIP。
 */
void task_dump_all(void);

#endif
