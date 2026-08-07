/*
 * syscall.c - 系统调用实现（int 0x80 入口）
 * eax=调用号，ebx/ecx/edx=参数，返回值写回 eax。
 */

#include "../include/syscall/syscall.h"
#include "../include/driver/vga.h"
#include "../include/driver/keyboard.h"
#include "../include/driver/mouse.h"
#include "../include/driver/ata.h"
#include "../include/driver/usb.h"
#include "../include/mm/vmm.h"
#include "../include/fs/fat.h"
#include "../include/fs/vfs.h"
#include "../include/proc/task.h"
#include "../include/driver/rtc.h"
#include <stdint.h>

int sys_putchar(char c) {
    vga_putchar(c);
    return 0;
}

/* 阻塞直到有按键 */
int sys_getchar(void) {
    return (int)keyboard_get_char();
}

/* 非阻塞取键：有键立即返回键值，无键返回 -1（TUI 事件循环轮询用）
 * ⚠️ 不用 keyboard_have_key()：键盘 IRQ 被屏蔽（纯轮询模式），
 * 环形缓冲只有阻塞版 get_char 才填充；这里直接轮询端口。 */
int sys_getkey_nb(void) {
    return (int)keyboard_get_char_nb();
}

/* 用户指针必须先验 PAGE_USER，否则恶意程序能骗内核读任意内存。
 * 逐页校验，最多 4096 字节。 */
int sys_write(const char *str) {
    if (!str) return -1;
    if (!vmm_is_user_accessible((uint32_t)str)) return -1;

    /* ⚠️ 旧实现不检查 NUL 终止，未终止的"字符串"会静默输出满 4096
     * 字节；现在先逐页扫描确认在用户空间内终止。 */
    bool terminated = false;
    for (uint32_t i = 0; i < 4096; i++) {
        /* 跨页边界时校验下一页 */
        if (i > 0 && (i & 0xFFF) == 0) {
            if (!vmm_is_user_accessible((uint32_t)(str + i))) break;
        }
        if (str[i] == '\0') { terminated = true; break; }
    }
    if (!terminated) return -1;

    /* 整串一把锁输出（vga_write 内部持锁），不再逐字符重复加锁。
     * NUL 已由上方扫描确认，输出不会越界。 */
    vga_write(str);
    return 0;
}

/* 标记终止并交给调度器清理（摘链表、释放 PCB/内核栈、切换走） */
void sys_exit(int status) {
    vga_write("[Syscall] Task exiting, status: ");
    vga_write_hex((uint32_t)status);
    vga_write("\n");
    /* ⚠️ 之前直接 hlt 挂死整机；现在 task_exit() 处理清理并切走 */
    task_exit();
    /* 理论不可达：task_exit 内部会切换走，不再返回 */
    while (1) __asm__ volatile ("hlt");
}

/* 读扇区（演示用）；缓冲须在用户页内，512B 可能跨页 */
static int sys_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!buffer) return 1;
    if (!vmm_is_user_accessible((uint32_t)buffer)) return 1;
    if (!vmm_is_user_accessible((uint32_t)buffer + 511)) return 1;
    return ata_read_sector(lba, buffer) ? 0 : 1;
}

/* 读鼠标：3×int32（x, y, buttons）写入用户缓冲 */
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

/* 定位光标（TUI 用），越界由 vga_set_cursor 截断 */
static int sys_set_cursor(uint32_t row, uint32_t col) {
    vga_set_cursor((int)row, (int)col);
    return 0;
}

/* 返回 (行 << 16) | 列 */
static int sys_get_cursor(void) {
    return (int)vga_get_cursor_pos();
}

static int sys_clear(void) {
    vga_clear();
    return 0;
}

static int sys_get_sens(void) {
    return mouse_get_sensitivity();
}

/* 灵敏度，内核侧限制 1~16 */
static int sys_set_sens(uint32_t sens) {
    mouse_set_sensitivity((int)sens);
    return 0;
}

static int sys_get_color(void) {
    return (int)vga_get_default_color();
}

/* 颜色属性字节，如 0x0F=黑底白字 */
static int sys_set_color(uint32_t color) {
    vga_set_default_color((uint8_t)color);
    return 0;
}

/* 把光标处设为保护边界：之前只读，光标/鼠标点击只能在边界之后移动。
 * shell 每打印完提示符调用一次，把编辑区限制在提示行以下。 */
static int sys_protect(void) {
    vga_protect_before_cursor();
    return 0;
}

static int sys_get_pglyph(void) {
    return (int)mouse_get_pointer_glyph();
}

static int sys_set_pglyph(uint32_t glyph) {
    mouse_set_pointer_glyph((uint8_t)glyph);
    return 0;
}

/* 写文件：校验文件名/数据都在用户页内后交给 VFS 路由到对应 FAT 实例 */
static int sys_write_file(uint32_t filename, uint32_t data, uint32_t size) {
    if (!filename || !data) return -1;
    if (!vmm_is_user_accessible(filename)) return -1;
    if (size > 65536) return -1;
    if (!vmm_is_user_accessible(data)) return -1;
    if (size > 0 && !vmm_is_user_accessible(data + size - 1)) return -1;

    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)filename, &sub);
    if (!fs) return -1;
    return fat_write_file(fs, sub, (const void*)data, size) ? 0 : -1;
}

/* 读文件：文件名/缓冲都在用户页内；返回实际字节数 */
static int sys_read_file(uint32_t filename, uint32_t buf, uint32_t max_size) {
    if (!filename || !buf) return -1;
    if (!vmm_is_user_accessible(filename)) return -1;
    if (max_size == 0) return -1;
    if (!vmm_is_user_accessible(buf)) return -1;
    if (!vmm_is_user_accessible(buf + max_size - 1)) return -1;

    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)filename, &sub);
    if (!fs) return -1;
    fat_dirent_t entry;
    if (!fat_find_file(fs, sub, &entry)) return -1;
    return (int)fat_load_file(fs, &entry, (void*)buf, max_size);
}

/* 删除文件/空目录 */
static int sys_delete_file(uint32_t filename) {
    if (!filename) return -1;
    if (!vmm_is_user_accessible(filename)) return -1;
    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)filename, &sub);
    if (!fs) return -1;
    return fat_delete_file(fs, sub) ? 0 : -1;
}

/* 创建子目录 */
static int sys_mkdir(uint32_t path) {
    if (!path) return -1;
    if (!vmm_is_user_accessible(path)) return -1;
    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)path, &sub);
    if (!fs) return -1;
    return fat_mkdir(fs, sub) ? 0 : -1;
}

/* 列目录：fat_dirent_t 数组写入用户缓冲（path 可为 NULL=根）
 * ⚠️ /dev 是虚拟设备目录（devfs）：不落 FAT，直接枚举已注册设备 */
static int sys_list(uint32_t path, uint32_t buf, uint32_t max) {
    if (!buf || max == 0) return -1;
    if (path && !vmm_is_user_accessible(path)) return -1;
    if (!vmm_is_user_accessible(buf)) return -1;
    if (!vmm_is_user_accessible(buf + (max - 1) * 32)) return -1;

    const char* p = path ? (const char*)path : NULL;
    /* /dev 虚拟目录：大小写不敏感匹配 */
    if (p && p[0] == '/' &&
        (p[1]=='d'||p[1]=='D') && (p[2]=='e'||p[2]=='E') &&
        (p[3]=='v'||p[3]=='V') && p[4] == '\0') {
        return (int)vfs_list_devices((fat_dirent_t*)buf, max);
    }

    const char* sub;
    fat_fs_t* fs = vfs_resolve(p, &sub);
    if (!fs) return -1;
    uint32_t dir_cluster = fat_open_dir(fs, sub);
    if (dir_cluster == 0xFFFFFFFF) return -1;
    return (int)fat_read_dir(fs, dir_cluster, (fat_dirent_t*)buf, max);
}

static int sys_tasks(void) {
    task_dump_all();
    return 0;
}

/* 返回 (时 << 16) | (分 << 8) | 秒 */
static int sys_get_time(void) {
    return (int)rtc_get_time_packed();
}

/* 返回 ((年-2000) << 16) | (月 << 8) | 日 */
static int sys_get_date(void) {
    return (int)rtc_get_date_packed();
}

static int sys_uptime(void) {
    return (int)rtc_get_uptime();
}

/* 打印枚举的 USB 设备列表与 MSC 状态（调试/验证用） */
static int sys_usb_info(void) {
    usb_device_t *dev = usb_get_device_list();
    int n = 0;
    while (dev) {
        vga_write("[USB] dev addr=");
        vga_write_hex(dev->address);
        vga_write(" VID=");
        vga_write_hex(dev->dev_desc.idVendor);
        vga_write(" PID=");
        vga_write_hex(dev->dev_desc.idProduct);
        vga_write(" class=");
        vga_write_hex(dev->dev_desc.bDeviceClass);
        vga_write("\n");
        dev = dev->next;
        n++;
    }
    vga_write("[USB] total devices: ");
    vga_write_hex(n);
    vga_write("\n");
    if (usb_msc_present()) {
        vga_write("[USB MSC] present, ");
        vga_write_hex(usb_msc_get_sector_count());
        vga_write(" sectors x 512B\n");
    } else {
        vga_write("[USB MSC] not present\n");
    }
    return 0;
}

/* 挂载设备（ebx=设备名, ecx=挂载点） */
static int sys_mount(uint32_t dev_name, uint32_t point) {
    if (!dev_name || !point) return -1;
    if (!vmm_is_user_accessible(dev_name) || !vmm_is_user_accessible(point)) return -1;
    return vfs_mount((const char*)point, (const char*)dev_name) ? 0 : -1;
}

/* 卸载（ebx=挂载点） */
static int sys_umount(uint32_t point) {
    if (!point) return -1;
    if (!vmm_is_user_accessible(point)) return -1;
    return vfs_umount((const char*)point) ? 0 : -1;
}

/* 打印设备/挂载表（内核态 vga 输出） */
static int sys_devices(void) {
    vfs_print_devices();
    return 0;
}

/* 按 regs->eax 分发；返回值写回 regs->eax，汇编 iret 后调用者从 eax 取 */
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
            /* ⚠️ 切回时走 switch_to_user 直接 iret 回用户态，不经过
             * 这里写返回值——此调用的返回值不可用，调用方应忽略。 */
            yield();
            ret = 0;
            break;

        case SYS_DELETE_FILE:
            /* ebx = 文件名 */
            ret = sys_delete_file(regs->ebx);
            break;

        case SYS_USB_INFO:
            ret = sys_usb_info();
            break;

        case SYS_MOUNT:
            /* ebx = 设备名, ecx = 挂载点 */
            ret = sys_mount(regs->ebx, regs->ecx);
            break;

        case SYS_UMOUNT:
            /* ebx = 挂载点 */
            ret = sys_umount(regs->ebx);
            break;

        case SYS_DEVICES:
            ret = sys_devices();
            break;

        case SYS_GETKEY_NB:
            ret = sys_getkey_nb();
            break;

        default:
            /* 未知调用号，多半是用户程序 bug */
            vga_write("[Syscall] Unknown number: ");
            vga_write_hex(regs->eax);
            vga_write("\n");
            ret = -1;
            break;
    }

    /* 写回 eax，用户态 popa 后即可拿到返回值 */
    regs->eax = ret;
}
