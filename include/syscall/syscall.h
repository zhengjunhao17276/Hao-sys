/*
 * syscall.h - 系统调用接口（int 0x80）
 * eax=调用号，ebx/ecx/edx=参数，返回值在 eax。
 * 用户态：__asm__ volatile ("int $0x80" : "=a"(ret) : "a"(nr), "b"(arg1));
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
#define SYS_MOUNT      28 /* 挂载设备（ebx=设备名, ecx=挂载点） */
#define SYS_UMOUNT     29 /* 卸载（ebx=挂载点） */
#define SYS_DEVICES    30 /* 打印设备/挂载表 */

/*
 * regs_t - 中断上下文（由 isr80_handler 压栈形成）
 * ⚠️ 字段顺序必须与 isr_asm.asm 的压栈顺序一致！
 * 压栈顺序：push gs/fs/es/ds → pusha（EAX 先压，占最高地址）。
 * 结构体偏移最小 = 最后压入 → EDI 在前、EAX 在后。
 */
typedef struct {
    uint32_t gs;      /* 偏移  0：push gs 最后压入 */
    uint32_t fs;      /* 偏移  4 */
    uint32_t es;      /* 偏移  8 */
    uint32_t ds;      /* 偏移 12 */
    uint32_t edi;     /* 偏移 16：pusha 最后压入 → 最低地址 */
    uint32_t esi;     /* 偏移 20 */
    uint32_t ebp;     /* 偏移 24 */
    uint32_t old_esp; /* 偏移 28：int 0x80 前的原始 SP */
    uint32_t ebx;     /* 偏移 32 */
    uint32_t edx;     /* 偏移 36 */
    uint32_t ecx;     /* 偏移 40 */
    uint32_t eax;     /* 偏移 44：pusha 最先压入 → 最高地址（调用号/返回值） */
} regs_t;

/* 按 regs->eax 分发；返回值写回 regs->eax，由 isr80_handler 调用 */
void syscall_dispatcher(regs_t *regs);

/* ---- 具体系统调用函数（供分发器调用） ---- */
int  sys_putchar(char c);
int  sys_getchar(void);
int  sys_write(const char *str);
void sys_exit(int status);          /* 退出（不会返回） */

#endif
