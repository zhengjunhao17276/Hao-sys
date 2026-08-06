/**
 * =========================================================================
 * syscall.h - 系统调用接口定义
 *
 * 系统调用（syscall）是用户态程序请求内核服务的标准方式。
 * HaoOS 使用 int 0x80 软中断实现系统调用，这是 Linux 经典做法。
 *
 * 调用约定：
 *   - eax = 系统调用号
 *   - ebx = 参数 1
 *   - ecx = 参数 2（可选）
 *   - edx = 参数 3（可选）
 *   - 返回值放在 eax 中
 *
 * 用户态使用方式（C 内联汇编）：
 *   int result;
 *   __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_PUTCHAR), "b"('A'));
 *
 * 寄存器上下文（由汇编中断处理程序压栈后传递给 C 分发器）：
 *   isr_asm.asm 中的 isr80_handler 会将所有段寄存器和通用寄存器
 *   压入栈中，形成 regs_t 结构体的布局，然后调用 syscall_dispatcher()。
 *
 * 当前实现的系统调用：
 *   SYS_PUTCHAR - 输出一个字符
 *   SYS_GETCHAR - 获取一个键盘输入
 *   SYS_WRITE   - 输出字符串
 *   SYS_EXIT    - 退出程序
 *   SYS_READ_SECT - 读磁盘扇区（预留演示用）
 * =========================================================================
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* ---- 系统调用号 ---- */
#define SYS_PUTCHAR   1   /* 向 VGA 终端输出一个字符 */
#define SYS_GETCHAR   2   /* 从键盘获取一个字符 */
#define SYS_WRITE     3   /* 向 VGA 终端输出字符串 */
#define SYS_EXIT      4   /* 退出当前程序 */
#define SYS_READ_SECT 5   /* 读磁盘扇区（预留，演示用） */
#define SYS_GETMOUSE  6   /* 获取鼠标状态（x, y, buttons） */
#define SYS_SET_CURSOR 7  /* 定位光标（ebx=行, ecx=列） */
#define SYS_GET_CURSOR 8  /* 读光标位置（eax=(行<<16)|列） */
#define SYS_CLEAR      9  /* 清屏 */
#define SYS_GET_SENS  10  /* 读鼠标灵敏度（eax=灵敏度） */
#define SYS_SET_SENS  11  /* 写鼠标灵敏度（ebx=灵敏度 1~16） */
#define SYS_GET_COLOR 12  /* 读默认颜色（eax=属性字节） */
#define SYS_SET_COLOR 13  /* 写默认颜色（ebx=属性字节 0~255） */
#define SYS_PROTECT   14  /* 锁定保护区域（光标只能在边界之后移动） */
#define SYS_GET_PGLYPH 15 /* 读鼠标指针图案（eax=字形码） */
#define SYS_SET_PGLYPH 16 /* 写鼠标指针图案（ebx=字形码） */
#define SYS_WRITE_FILE 17 /* 写文件（ebx=文件名, ecx=数据, edx=长度） */
#define SYS_READ_FILE  18 /* 读文件（ebx=文件名, ecx=缓冲, edx=最大长度） */
#define SYS_TASKS      19 /* 打印任务列表 */
#define SYS_GET_TIME   20 /* 读时间（eax=(时<<16)|(分<<8)|秒） */
#define SYS_GET_DATE   21 /* 读日期（eax=((年-2000)<<16)|(月<<8)|日） */
#define SYS_DELETE_FILE 22 /* 删除文件（ebx=文件名） */
#define SYS_UPTIME     23 /* 读开机时长（秒） */
#define SYS_MKDIR      24 /* 创建子目录（ebx=路径） */
#define SYS_LIST       25 /* 列目录（ebx=路径, ecx=目录项数组, edx=最大数） */
#define SYS_YIELD      26 /* 让出 CPU（协作式调度；返回值不可用） */
#define SYS_USB_INFO   27 /* 打印 USB 设备列表 + MSC 状态 */

/**
 * regs_t - 寄存器上下文结构体
 *
 * ⚠️ 字段顺序必须与 isr_asm.asm 中 isr80_handler 的压栈顺序一致！
 *
 * 栈布局（从低地址到高地址）：
 *   [gs] [fs] [es] [ds] ← 后压入的段寄存器
 *   [EDI] [ESI] [EBP] [ESP] [EBX] [EDX] [ECX] [EAX] ← pusha 顺序
 *
 * pusha 指令先压 EAX（最高地址），后压 EDI（最低地址）。
 * 在 C 结构体中，偏移最小的字段 = 最低地址 = 最后压入的值。
 * 所以通用寄存器部分要**反着写**：EDI 在最前面，EAX 在最后。
 */
typedef struct {
    uint32_t gs;      /* 偏移  0：段寄存器 GS（push gs 最后压入） */
    uint32_t fs;      /* 偏移  4：段寄存器 FS */
    uint32_t es;      /* 偏移  8：段寄存器 ES */
    uint32_t ds;      /* 偏移 12：段寄存器 DS */
    uint32_t edi;     /* 偏移 16：pusha 最后压入 → 在最低地址 */
    uint32_t esi;     /* 偏移 20 */
    uint32_t ebp;     /* 偏移 24 */
    uint32_t old_esp; /* 偏移 28：pusha 压入的原始 ESP（发出 int 0x80 前的 SP） */
    uint32_t ebx;     /* 偏移 32 */
    uint32_t edx;     /* 偏移 36 */
    uint32_t ecx;     /* 偏移 40 */
    uint32_t eax;     /* 偏移 44：pusha 最先压入 → 在最高地址（系统调用号/返回值） */
} regs_t;

/**
 * syscall_dispatcher - 系统调用分发器
 * @regs: 寄存器上下文指针
 *
 * 根据 regs->eax 中的系统调用号分发到具体的处理函数，
 * 处理完成后将返回值写回 regs->eax，这样汇编代码 popa
 * 后返回值会出现在调用者的 eax 中。
 *
 * 此函数由 isr80_handler（汇编）调用。
 */
void syscall_dispatcher(regs_t *regs);

/* ---- 具体系统调用函数（供分发器调用） ---- */
int  sys_putchar(char c);           /* 输出字符 */
int  sys_getchar(void);             /* 获取字符 */
int  sys_write(const char *str);    /* 输出字符串 */
void sys_exit(int status);          /* 退出（不会返回） */

#endif
