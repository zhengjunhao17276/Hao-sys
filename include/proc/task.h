/*
 * task.h - 任务/进程管理接口（协作式调度）
 *
 * ⚠️ esp/eip 必须在偏移 0/4：switch.asm 按 [task+0]/[task+4] 直接访问。
 * GDT：0=null，1=内核代码(0x08)，2=内核数据(0x10)，3=用户代码(0x1B)，
 * 4=用户数据(0x23)，5=TSS(0x28)。用户态中断时 CPU 从 TSS 取 esp0 切内核栈。
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

/* 调度查找就绪任务的上限，防死循环 */
#define MAX_TASKS 32

/* ---- 任务状态枚举 ---- */
typedef enum {
    TASK_RUNNING,      /* 当前正在运行 */
    TASK_READY,        /* 就绪，等待调度 */
    TASK_BLOCKED,      /* 阻塞（等待某事件） */
    TASK_TERMINATED    /* 已终止，等待被清理 */
} task_state_t;

/* PCB。⚠️ esp/eip 必须占偏移 0/4：switch.asm 按固定偏移访问 */
typedef struct task_struct {
    uint32_t esp;           /* 偏移 0：栈指针 */
    uint32_t eip;           /* 偏移 4：指令指针 */

    uint32_t pid;
    task_state_t state;
    uint32_t ticks_used;    /* 时间片已用 tick 数（预留抢占用） */

    uint32_t kernel_esp0;   /* 用户态中断栈顶，调度时写入 TSS.esp0 */

    /* ⚠️ 不能用 kernel_esp0 判断任务类型（内核任务也有内核栈）。
     * 用户任务：esp 指向 [pusha 帧][iret 帧] 复合帧，switch_to_user 恢复；
     * 内核任务：esp 指向 [pusha 帧][EBP][entry]，switch_to 恢复 */
    bool is_user;           /* true=用户态任务 */

    uint32_t* page_directory;  /* 页目录：用户任务各自独立，内核任务为内核目录 */

    /* 用户态映射页登记：任务退出时逐一 unmap + 释放物理页防泄漏。
     * 上限 = load_and_run_shell 的 64 页保险丝 + 1 页用户栈 */
    uint32_t user_virt_pages[65];
    uint32_t user_virt_count;

    struct task_struct* next;  /* 单向链表 */

    char name[32];
} task_t;

/* ---- 函数声明 ---- */

/* 当前任务。非 static：isr_asm.asm / pit.c 要访问 */
extern task_t* current_task;

/* 初始化 GDT/TSS 并创建 idle 任务 */
void task_init(void);

/* 创建内核任务，失败返回 NULL。首次切换时 ret 直接跳 entry */
task_t* task_create(const char* name, void (*entry)(void));

/* 创建用户任务，失败返回 NULL。首次切换经 iret 进 ring3 */
task_t* task_create_user(const char* name, void* entry, void* stack);

/* 设置 TSS.esp0。不经调度器直接 iret 进用户态前必须调，
 * 否则中断时 CPU 用 esp0=0 压栈直接崩 */
void task_set_kernel_stack(uint32_t esp0);

/* 主动让出 CPU */
void yield(void);

/* 轮转调度：选下一个就绪任务切换（顺带回收终止任务） */
void schedule(void);

/* 返回当前任务指针 */
task_t* task_current(void);

/* 终止当前任务（标记后等调度器回收） */
void task_exit(void);

/* 调试：打印所有任务状态 */
void task_dump_all(void);

#endif
