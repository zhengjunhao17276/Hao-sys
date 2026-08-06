/**
 * =========================================================================
 * syscall.c - 系统调用实现
 *
 * 系统调用是用户态程序请求内核服务的入口。HaoOS 使用 int 0x80 软中断
 * 实现系统调用，这是经典 UNIX/Linux 的做法。
 *
 * 调用序号和参数约定：
 *   - eax = 系统调用号
 *   - ebx = 参数 1
 *   - ecx = 参数 2（可选）
 *   - edx = 参数 3（可选）
 *   - 返回值在 eax 中
 *
 * 分发流程：
 *   isr80_handler（汇编）→ pusha/保存段寄存器 → syscall_dispatcher()
 *   → 根据 eax 分发 → 写入 eax 返回值 → popa/ret → 返回用户态
 *
 * 当前实现的系统调用：
 *   SYS_PUTCHAR (1)  - 输出字符到 VGA 终端
 *   SYS_GETCHAR (2)  - 从键盘获取字符（阻塞）
 *   SYS_WRITE   (3)  - 输出字符串到 VGA 终端
 *   SYS_EXIT    (4)  - 退出当前程序（无限 HLT）
 *   SYS_READ_SECT (5) - 读取磁盘扇区（预留演示）
 * =========================================================================
 */

#include "../include/syscall/syscall.h"
#include "../include/driver/vga.h"
#include "../include/driver/keyboard.h"
#include "../include/driver/mouse.h"
#include "../include/driver/ata.h"
#include "../include/mm/vmm.h"
#include "../include/fs/fat.h"
#include "../include/proc/task.h"
#include "../include/driver/rtc.h"
#include <stdint.h>

/* ==================== 具体系统调用实现 ==================== */

/**
 * sys_putchar - 向 VGA 终端输出一个字符
 */
int sys_putchar(char c) {
    vga_putchar(c);
    return 0;
}

/**
 * sys_getchar - 从键盘获取一个字符（阻塞等待）
 *
 * 调用 keyboard_get_char() 会阻塞直到用户按下按键。
 * 此函数不检查按键是否来自哪个键盘——它只是等待。
 */
int sys_getchar(void) {
    return (int)keyboard_get_char();
}

/**
 * sys_write - 向 VGA 终端输出字符串
 *
 * 安全校验：用户传来的指针不能直接解引用——必须先确认它指向的
 * 是用户可访问的页（PAGE_USER），否则恶意用户程序可以让内核
 * 读取任意内核内存（信息泄露）。逐页校验，最多输出 4096 字节。
 */
int sys_write(const char *str) {
    if (!str) return -1;
    if (!vmm_is_user_accessible((uint32_t)str)) return -1;

    for (uint32_t i = 0; i < 4096; i++) {
        /* 跨页边界时校验下一页 */
        if (i > 0 && (i & 0xFFF) == 0) {
            if (!vmm_is_user_accessible((uint32_t)(str + i))) return -1;
        }
        char c = str[i];
        if (c == '\0') break;
        vga_putchar(c);
    }
    return 0;
}

/**
 * sys_exit - 退出程序
 * @status: 退出状态码
 *
 * 当前实现是输出状态码后进入无限 HLT 循环。
 * 真正的 exit 应该由调度器从任务链表移除，此处简化了。
 */
void sys_exit(int status) {
    vga_write("[Syscall] System halt requested. Status: ");
    vga_write_hex((uint32_t)status);
    vga_write("\n");
    while (1) __asm__ volatile ("hlt");
}

/**
 * sys_read_sector - 读取磁盘扇区（预留演示功能）
 * @lba:    扇区号
 * @buffer: 缓冲区
 * 返回 0=成功，1=失败
 */
static int sys_read_sector(uint32_t lba, uint8_t *buffer) {
    /* 校验目标缓冲区在用户可访问的页内（扇区 512 字节可能跨页） */
    if (!buffer) return 1;
    if (!vmm_is_user_accessible((uint32_t)buffer)) return 1;
    if (!vmm_is_user_accessible((uint32_t)buffer + 511)) return 1;
    return ata_read_sector(lba, buffer) ? 0 : 1;
}

/**
 * sys_getmouse - 获取鼠标状态
 * @buf: 用户缓冲区指针（3 × int32 = x, y, buttons）
 * 返回 0=成功，-1=无效指针
 */
static int sys_getmouse(uint32_t buf) {
    if (!buf) return -1;
    /* 校验 3 个 int32（12 字节）都在用户可访问的页内 */
    if (!vmm_is_user_accessible(buf)) return -1;
    if (!vmm_is_user_accessible(buf + 8)) return -1;
    int x, y, buttons;
    mouse_get_packet(&x, &y, &buttons);
    volatile uint32_t *dest = (volatile uint32_t*)buf;
    dest[0] = (uint32_t)x;
    dest[1] = (uint32_t)y;
    dest[2] = (uint32_t)buttons;
    return 0;
}

/* ==================== settings TUI 支持的系统调用 ==================== */

/**
 * sys_set_cursor - 定位 VGA 光标（TUI 绘制用）
 * @row: 行（0~24）
 * @col: 列（0~79）
 * 越界由 vga_set_cursor 自动截断。
 */
static int sys_set_cursor(uint32_t row, uint32_t col) {
    vga_set_cursor((int)row, (int)col);
    return 0;
}

/**
 * sys_get_cursor - 读取 VGA 光标位置
 * @return (行 << 16) | 列
 */
static int sys_get_cursor(void) {
    return (int)vga_get_cursor_pos();
}

/**
 * sys_clear - 清屏（TUI 用）
 */
static int sys_clear(void) {
    vga_clear();
    return 0;
}

/**
 * sys_get_sens - 读取鼠标灵敏度
 */
static int sys_get_sens(void) {
    return mouse_get_sensitivity();
}

/**
 * sys_set_sens - 设置鼠标灵敏度
 * @sens: 目标灵敏度（内核侧自动限制 1~16）
 */
static int sys_set_sens(uint32_t sens) {
    mouse_set_sensitivity((int)sens);
    return 0;
}

/**
 * sys_get_color - 读取默认颜色（属性字节）
 */
static int sys_get_color(void) {
    return (int)vga_get_default_color();
}

/**
 * sys_set_color - 设置默认颜色（属性字节）
 * @color: 0~255，例如 0x0F=黑底白字
 */
static int sys_set_color(uint32_t color) {
    vga_set_default_color((uint8_t)color);
    return 0;
}

/**
 * sys_protect - 锁定保护区域
 *
 * 把当前光标位置设为保护边界：边界之前的内容只读，
 * 文本光标（含鼠标点击放置）只能在边界之后移动。
 * shell 在每次打印提示符后调用，把编辑区限制在提示行以下。
 */
static int sys_protect(void) {
    vga_protect_before_cursor();
    return 0;
}

/**
 * sys_get_pglyph - 读取鼠标指针图案
 */
static int sys_get_pglyph(void) {
    return (int)mouse_get_pointer_glyph();
}

/**
 * sys_set_pglyph - 设置鼠标指针图案
 * @glyph: CP437 字形码（0~255）
 */
static int sys_set_pglyph(uint32_t glyph) {
    mouse_set_pointer_glyph((uint8_t)glyph);
    return 0;
}

/**
 * sys_write_file - 写文件（新建）
 * @filename: 用户态文件名指针
 * @data:     用户态数据指针
 * @size:     数据长度
 * 返回 0=成功，-1=失败
 *
 * 校验文件名和数据范围都是用户可访问的页后，交给 FAT 驱动写入。
 */
static int sys_write_file(uint32_t filename, uint32_t data, uint32_t size) {
    if (!filename || !data) return -1;
    if (!vmm_is_user_accessible(filename)) return -1;
    /* 校验数据起始页和结尾页（最多 64KB） */
    if (size > 65536) return -1;
    if (!vmm_is_user_accessible(data)) return -1;
    if (size > 0 && !vmm_is_user_accessible(data + size - 1)) return -1;

    return fat_write_file((const char*)filename, (const void*)data, size) ? 0 : -1;
}

/**
 * sys_read_file - 读文件
 * @filename: 用户态文件名指针
 * @buf:      用户态缓冲区指针
 * @max_size: 缓冲区最大容量
 * 返回实际读取的字节数，失败返回 -1
 */
static int sys_read_file(uint32_t filename, uint32_t buf, uint32_t max_size) {
    if (!filename || !buf) return -1;
    if (!vmm_is_user_accessible(filename)) return -1;
    if (max_size == 0) return -1;
    if (!vmm_is_user_accessible(buf)) return -1;
    if (!vmm_is_user_accessible(buf + max_size - 1)) return -1;

    fat_dirent_t entry;
    if (!fat_find_file((const char*)filename, &entry)) return -1;
    return (int)fat_load_file(&entry, (void*)buf, max_size);
}

/**
 * sys_delete_file - 删除文件或空目录
 * @filename: 用户态路径指针
 * 返回 0=成功，-1=失败（文件不存在 / 非空目录）
 */
static int sys_delete_file(uint32_t filename) {
    if (!filename) return -1;
    if (!vmm_is_user_accessible(filename)) return -1;
    return fat_delete_file((const char*)filename) ? 0 : -1;
}

/**
 * sys_mkdir - 创建子目录
 * @path: 用户态路径指针
 * 返回 0=成功，-1=失败
 */
static int sys_mkdir(uint32_t path) {
    if (!path) return -1;
    if (!vmm_is_user_accessible(path)) return -1;
    return fat_mkdir((const char*)path) ? 0 : -1;
}

/**
 * sys_list - 列出目录内容
 * @path:   用户态路径指针（NULL/空 = 根目录）
 * @buf:    用户态 fat_dirent_t 数组
 * @max:    最大条目数
 * 返回条目数，失败返回 -1
 */
static int sys_list(uint32_t path, uint32_t buf, uint32_t max) {
    if (!buf || max == 0) return -1;
    if (path && !vmm_is_user_accessible(path)) return -1;
    if (!vmm_is_user_accessible(buf)) return -1;
    if (!vmm_is_user_accessible(buf + (max - 1) * 32)) return -1;

    uint32_t dir_cluster = fat_open_dir(path ? (const char*)path : NULL);
    if (dir_cluster == 0xFFFFFFFF) return -1;
    return (int)fat_read_dir(dir_cluster, (fat_dirent_t*)buf, max);
}

/**
 * sys_tasks - 打印任务列表（内核直接输出）
 */
static int sys_tasks(void) {
    task_dump_all();
    return 0;
}

/**
 * sys_get_time - 读取当前时间（打包）
 * @return (时 << 16) | (分 << 8) | 秒
 */
static int sys_get_time(void) {
    return (int)rtc_get_time_packed();
}

/**
 * sys_get_date - 读取当前日期（打包）
 * @return ((年-2000) << 16) | (月 << 8) | 日
 */
static int sys_get_date(void) {
    return (int)rtc_get_date_packed();
}

/**
 * sys_uptime - 读取自开机以来的秒数
 */
static int sys_uptime(void) {
    return (int)rtc_get_uptime();
}

/* ==================== 分发器 ==================== */

/**
 * syscall_dispatcher - 系统调用分发器
 * @regs: 中断上下文保存的寄存器值（见 regs_t 结构）
 *
 * 根据 regs->eax 中的系统调用号分发到对应的处理函数。
 * 处理完成后将返回值写回 regs->eax——汇编代码在 iret 返回
 * 后，调用者可以通过 eax 获取返回值。
 *
 * 如果系统调用号未识别，打印警告并返回 -1。
 */
void syscall_dispatcher(regs_t *regs) {
    int ret = -1;

    switch (regs->eax) {
        case SYS_PUTCHAR:
            /* ebx = 字符（低 8 位有效） */
            ret = sys_putchar((char)(regs->ebx & 0xFF));
            break;

        case SYS_GETCHAR:
            ret = sys_getchar();
            break;

        case SYS_WRITE:
            /* ebx = 字符串指针 */
            ret = sys_write((const char*)regs->ebx);
            break;

        case SYS_EXIT:
            /* ebx = 退出状态码 */
            sys_exit((int)regs->ebx);
            /* 不会返回 */
            break;

        case SYS_READ_SECT:
            /* ebx = lba, ecx = buffer */
            ret = sys_read_sector(regs->ebx, (uint8_t*)regs->ecx);
            break;

        case SYS_GETMOUSE:
            /* ebx = 用户缓冲区指针（3 × int32） */
            ret = sys_getmouse(regs->ebx);
            break;

        case SYS_SET_CURSOR:
            /* ebx = 行, ecx = 列 */
            ret = sys_set_cursor(regs->ebx, regs->ecx);
            break;

        case SYS_GET_CURSOR:
            ret = sys_get_cursor();
            break;

        case SYS_CLEAR:
            ret = sys_clear();
            break;

        case SYS_GET_SENS:
            ret = sys_get_sens();
            break;

        case SYS_SET_SENS:
            /* ebx = 灵敏度 */
            ret = sys_set_sens(regs->ebx);
            break;

        case SYS_GET_COLOR:
            ret = sys_get_color();
            break;

        case SYS_SET_COLOR:
            /* ebx = 颜色属性字节 */
            ret = sys_set_color(regs->ebx);
            break;

        case SYS_PROTECT:
            ret = sys_protect();
            break;

        case SYS_GET_PGLYPH:
            ret = sys_get_pglyph();
            break;

        case SYS_SET_PGLYPH:
            /* ebx = 指针图案字形码 */
            ret = sys_set_pglyph(regs->ebx);
            break;

        case SYS_WRITE_FILE:
            /* ebx = 文件名, ecx = 数据, edx = 长度 */
            ret = sys_write_file(regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_READ_FILE:
            /* ebx = 文件名, ecx = 缓冲, edx = 最大长度 */
            ret = sys_read_file(regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_TASKS:
            ret = sys_tasks();
            break;

        case SYS_GET_TIME:
            ret = sys_get_time();
            break;

        case SYS_GET_DATE:
            ret = sys_get_date();
            break;

        case SYS_UPTIME:
            ret = sys_uptime();
            break;

        case SYS_MKDIR:
            /* ebx = 路径 */
            ret = sys_mkdir(regs->ebx);
            break;

        case SYS_LIST:
            /* ebx = 路径, ecx = 目录项数组, edx = 最大数 */
            ret = sys_list(regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_YIELD:
            /* 让出 CPU（协作式调度测试用）。
             * ⚠️ 注意：被调度器切回时走 switch_to_user 直接 iret 回
             * 用户态，不会经过这里写返回值——所以此调用的返回值
             * 不可用，调用方应忽略。 */
            yield();
            ret = 0;
            break;

        case SYS_DELETE_FILE:
            /* ebx = 文件名 */
            ret = sys_delete_file(regs->ebx);
            break;

        default:
            /* 未知系统调用号——可能用户程序有 bug */
            vga_write("[Syscall] Unknown number: ");
            vga_write_hex(regs->eax);
            vga_write("\n");
            ret = -1;
            break;
    }

    /* 返回值写回 eax，这样用户态通过 popa 可以获取到 */
    regs->eax = ret;
}
