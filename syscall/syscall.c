/*
 * syscall.c - 系统调用实现（int 0x80 入口）
 * eax=调用号，ebx/ecx/edx=参数，返回值写回 eax。
 */

#include "../include/syscall/syscall.h"
#include "../include/driver/vga.h"
#include "../include/driver/io.h"
#include "../include/driver/keyboard.h"
#include "../include/driver/mouse.h"
#include "../include/driver/ata.h"
#include "../include/driver/usb.h"
#include "../include/mm/vmm.h"
#include "../include/fs/fat.h"
#include "../include/fs/vfs.h"
#include "../include/proc/task.h"
#include "../include/driver/rtc.h"
#include "../include/lib/string.h"
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

/* 光标可见性：ebx=0 隐藏、1=显示（全屏 TUI 界面用，消除硬件光标闪烁） */
static int sys_cursor_visible(uint32_t on) {
    if (on) vga_enable_cursor();
    else vga_disable_cursor();
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
/* ---- Linux 权限检查（mask：4=读 2=写 1=执行；root(uid 0) 全放行） ---- */

/* 子路径的父目录子路径：sub="a/b/c" → "a/b"；无 '/' 或空 → NULL（根目录） */
static const char* sys_parent_sub(const char* sub, char* buf, unsigned int size) {
    if (!sub || !sub[0]) return NULL;
    const char* last = NULL;
    for (const char* q = sub; *q; q++) if (*q == '/') last = q;
    if (!last) return NULL;
    unsigned int len = (unsigned int)(last - sub);
    if (len == 0) return NULL;
    if (len >= size) len = size - 1;
    for (unsigned int i = 0; i < len; i++) buf[i] = sub[i];
    buf[len] = '\0';
    return buf;
}

/* 合成根目录项：全零 + 目录属性 → fat_entry_unix_meta 默认 0755 root */
static void sys_root_entry(fat_dirent_t* e) {
    for (int i = 0; i < 32; i++) ((uint8_t*)e)[i] = 0;
    e->attributes = 0x10;
}

/* 单条目权限检查：euid==uid → 属主位，egid==gid → 组位，否则其他位 */
static int sys_check_entry(const fat_dirent_t* e, uint8_t mask) {
    uint16_t mode, uid, gid;
    fat_entry_unix_meta(e, &mode, &uid, &gid);
    uint32_t euid = current_task ? current_task->euid : 0;
    uint32_t egid = current_task ? current_task->egid : 0;
    if (euid == 0) return 0;                    /* root 绕过（Linux 语义） */
    int shift;
    if (euid == uid) shift = 6;
    else if (egid == gid) shift = 3;
    else shift = 0;
    uint8_t bits = (uint8_t)((mode >> shift) & 7);
    return (bits & mask) == mask ? 0 : -1;
}

/* 父目录写权限检查（Linux：增删文件都需要父目录写权限） */
static int sys_check_parent_writable(fat_fs_t* fs, const char* sub) {
    char pbuf[64];
    const char* p = sys_parent_sub(sub, pbuf, sizeof(pbuf));
    if (!p) {
        fat_dirent_t root;
        sys_root_entry(&root);
        return sys_check_entry(&root, 2);
    }
    fat_dirent_t pe;
    if (!fat_find_file(fs, p, &pe)) return -1;
    return sys_check_entry(&pe, 2);
}

static int sys_write_file(uint32_t filename, uint32_t data, uint32_t size) {
    if (!filename || !data) return -1;
    if (!vmm_is_user_accessible(filename)) return -1;
    if (size > 65536) return -1;
    if (!vmm_is_user_accessible(data)) return -1;
    if (size > 0 && !vmm_is_user_accessible(data + size - 1)) return -1;

    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)filename, &sub);
    if (!fs) return -1;

    /* 权限：已存在 → 需文件写权限；新建 → 需父目录写权限 */
    fat_dirent_t entry;
    bool existed = fat_find_file(fs, sub, &entry);
    uint16_t om = 0, ou = 0, og = 0;
    if (existed) {
        if (sys_check_entry(&entry, 2) != 0) return -1;
        fat_entry_unix_meta(&entry, &om, &ou, &og);   /* 覆盖会重建目录项，事后恢复 meta */
    } else {
        if (sys_check_parent_writable(fs, sub) != 0) return -1;
    }

    if (!fat_write_file(fs, sub, (const void*)data, size)) return -1;

    /* 新建：默认 0644 + 当前任务属主；覆盖：恢复原 meta */
    uint32_t cuid = current_task ? current_task->uid : 0;
    uint32_t cgid = current_task ? current_task->gid : 0;
    if (existed) fat_set_file_meta(fs, sub, om, ou, og);
    else fat_set_file_meta(fs, sub, S_IFREG | 0644, (uint16_t)cuid, (uint16_t)cgid);
    return 0;
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
    if (sys_check_entry(&entry, 4) != 0) return -1;   /* 读权限 */
    return (int)fat_load_file(fs, &entry, (void*)buf, max_size);
}

/* 带偏移读文件（分块复制大文件用）：ebx=文件名, ecx=偏移, edx=缓冲, esi=最大长度 */
static int sys_read_file_off(uint32_t filename, uint32_t offset, uint32_t buf, uint32_t max_size) {
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
    if (sys_check_entry(&entry, 4) != 0) return -1;   /* 读权限 */
    return (int)fat_load_file_off(fs, &entry, offset, (void*)buf, max_size);
}

/* 删除文件/空目录 */
static int sys_delete_file(uint32_t filename) {
    if (!filename) return -1;
    if (!vmm_is_user_accessible(filename)) return -1;
    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)filename, &sub);
    if (!fs) return -1;
    if (sys_check_parent_writable(fs, sub) != 0) return -1;   /* 父目录写权限 */
    return fat_delete_file(fs, sub) ? 0 : -1;
}

/* 创建子目录 */
static int sys_mkdir(uint32_t path) {
    if (!path) return -1;
    if (!vmm_is_user_accessible(path)) return -1;
    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)path, &sub);
    if (!fs) return -1;
    if (sys_check_parent_writable(fs, sub) != 0) return -1;   /* 父目录写权限 */
    if (!fat_mkdir(fs, sub)) return -1;
    /* 新目录：默认 0755 + 当前任务属主 */
    uint32_t cuid = current_task ? current_task->uid : 0;
    uint32_t cgid = current_task ? current_task->gid : 0;
    fat_set_file_meta(fs, sub, S_IFDIR | 0755, (uint16_t)cuid, (uint16_t)cgid);
    return 0;
}

/* 关机：先试 QEMU/Bochs 电源控制端口，再 cli+hlt 兜底（真机停在 HLT） */
static void sys_poweroff(void) {
    /* QEMU/Bochs 专有电源端口（0x604 写 0x2000 触发虚拟机关机；
     * 真机上该端口通常不存在，写入无副作用） */
    outw(0x604, 0x2000);

    /* 兜底：关中断 + HLT，等同断电前状态 */
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* 重启：8042 复位口（0x64←0xFE 触发 CPU 复位，QEMU/真机通用）；
 * 复位未生效则 cli+hlt 兜底 */
static void sys_reboot(void) {
    outb(0x64, 0xFE);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ---- 凭据：uid/gid/euid/egid ---- */
static int sys_getuid(void)  { return current_task ? (int)current_task->uid  : 0; }
static int sys_getgid(void)  { return current_task ? (int)current_task->gid  : 0; }
static int sys_geteuid(void) { return current_task ? (int)current_task->euid : 0; }
static int sys_getegid(void) { return current_task ? (int)current_task->egid : 0; }

/* setuid：root 任意设（uid+euid）；非 root 只能把 euid 设回真实 uid */
static int sys_setuid(uint32_t uid) {
    if (!current_task) return -1;
    if (current_task->euid == 0) { current_task->uid = current_task->euid = uid; return 0; }
    if (uid == current_task->uid) { current_task->euid = uid; return 0; }
    return -1;
}

static int sys_setgid(uint32_t gid) {
    if (!current_task) return -1;
    if (current_task->euid == 0) { current_task->gid = current_task->egid = gid; return 0; }
    if (gid == current_task->gid) { current_task->egid = gid; return 0; }
    return -1;
}

/* ---- stat / chmod / chown ---- */
static int sys_stat(uint32_t path, uint32_t buf) {
    if (!path || !buf) return -1;
    if (!vmm_is_user_accessible(path)) return -1;
    if (!vmm_is_user_accessible(buf) || !vmm_is_user_accessible(buf + 15)) return -1;

    const char* p = (const char*)path;
    stat_info_t st;

    /* 虚拟目录 /dev /mnt：合成 0755 root 目录 */
    if (p[0] == '/' &&
        (((p[1]=='d'||p[1]=='D') && (p[2]=='e'||p[2]=='E') && (p[3]=='v'||p[3]=='V') && p[4]=='\0') ||
         ((p[1]=='m'||p[1]=='M') && (p[2]=='n'||p[2]=='N') && (p[3]=='t'||p[3]=='T') && p[4]=='\0'))) {
        st.mode = S_IFDIR | 0755; st.uid = 0; st.gid = 0; st.size = 0;
        memcpy((void*)buf, &st, sizeof(st));
        return 0;
    }

    const char* sub;
    fat_fs_t* fs = vfs_resolve(p, &sub);
    if (!fs) return -1;
    fat_dirent_t entry;
    if (!fat_find_file(fs, sub, &entry)) return -1;
    fat_entry_unix_meta(&entry, &st.mode, &st.uid, &st.gid);
    st.size = entry.file_size;
    memcpy((void*)buf, &st, sizeof(st));
    return 0;
}

/* chmod：仅属主或 root；只改权限位（r/w/x + suid/sgid/sticky），类型位保持 */
static int sys_chmod(uint32_t path, uint32_t mode) {
    if (!path) return -1;
    if (!vmm_is_user_accessible(path)) return -1;
    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)path, &sub);
    if (!fs) return -1;
    fat_dirent_t entry;
    if (!fat_find_file(fs, sub, &entry)) return -1;

    uint16_t m, u, g;
    fat_entry_unix_meta(&entry, &m, &u, &g);
    uint32_t euid = current_task ? current_task->euid : 0;
    if (euid != 0 && euid != u) return -1;   /* 非属主非 root */

    uint16_t newmode = (uint16_t)((m & S_IFMT) | (mode & 07777));
    return fat_set_file_meta(fs, sub, newmode, u, g) ? 0 : -1;
}

/* chown：仅 root；uid/gid=0xFFFF 表示保持原值 */
static int sys_chown(uint32_t path, uint32_t uid, uint32_t gid) {
    if (!path) return -1;
    if (!vmm_is_user_accessible(path)) return -1;
    if (!current_task || current_task->euid != 0) return -1;

    const char* sub;
    fat_fs_t* fs = vfs_resolve((const char*)path, &sub);
    if (!fs) return -1;
    fat_dirent_t entry;
    if (!fat_find_file(fs, sub, &entry)) return -1;

    uint16_t m, u, g;
    fat_entry_unix_meta(&entry, &m, &u, &g);
    if (uid != 0xFFFF) u = (uint16_t)uid;
    if (gid != 0xFFFF) g = (uint16_t)gid;
    return fat_set_file_meta(fs, sub, m, u, g) ? 0 : -1;
}

/* 列目录：fat_dirent_t 数组写入用户缓冲（path 可为 NULL=根）
 * ⚠️ /dev 是虚拟设备目录（devfs）：不落 FAT，直接枚举已注册设备 */
static int sys_list(uint32_t path, uint32_t buf, uint32_t max) {
    if (!buf || max == 0) return -1;
    if (path && !vmm_is_user_accessible(path)) return -1;
    if (!vmm_is_user_accessible(buf)) return -1;
    if (!vmm_is_user_accessible(buf + (max - 1) * 32)) return -1;

    const char* p = path ? (const char*)path : NULL;
    /* /dev 虚拟目录：大小写不敏感匹配 → 枚举设备 */
    if (p && p[0] == '/' &&
        (p[1]=='d'||p[1]=='D') && (p[2]=='e'||p[2]=='E') &&
        (p[3]=='v'||p[3]=='V') && p[4] == '\0') {
        return (int)vfs_list_devices((fat_dirent_t*)buf, max);
    }
    /* /mnt 虚拟目录 → 列出已挂载的非根挂载点 */
    if (p && p[0] == '/' &&
        (p[1]=='m'||p[1]=='M') && (p[2]=='n'||p[2]=='N') &&
        (p[3]=='t'||p[3]=='T') && p[4] == '\0') {
        return (int)vfs_list_mount_points((fat_dirent_t*)buf, max);
    }

    const char* sub;
    fat_fs_t* fs = vfs_resolve(p, &sub);
    if (!fs) return -1;
    /* 读目录权限检查（root 绕过；虚拟目录已提前返回） */
    if (sub && sub[0]) {
        fat_dirent_t de;
        if (!fat_find_file(fs, sub, &de)) return -1;
        if (sys_check_entry(&de, 4) != 0) return -1;
    } else {
        fat_dirent_t root;
        sys_root_entry(&root);
        if (sys_check_entry(&root, 4) != 0) return -1;
    }
    uint32_t dir_cluster = fat_open_dir(fs, sub);
    if (dir_cluster == 0xFFFFFFFF) return -1;
    int n = (int)fat_read_dir(fs, dir_cluster, (fat_dirent_t*)buf, max);

    /* 根目录：追加虚拟目录项 DEV、MNT（系统挂载出来的，不在硬盘上） */
    if (n >= 0 && (!p || (p[0] == '/' && p[1] == '\0'))) {
        int total = n;
        /* DEV */
        if (total < (int)max) {
            fat_dirent_t* e = &((fat_dirent_t*)buf)[total];
            for (int j = 0; j < 32; j++) ((uint8_t*)e)[j] = 0;
            const char* dn = "DEV         ";
            for (int j = 0; j < 11; j++) e->name[j] = (uint8_t)dn[j];
            e->attributes = 0x10;   /* 目录 */
            total++;
        }
        /* MNT */
        if (total < (int)max) {
            fat_dirent_t* e = &((fat_dirent_t*)buf)[total];
            for (int j = 0; j < 32; j++) ((uint8_t*)e)[j] = 0;
            const char* mn = "MNT         ";
            for (int j = 0; j < 11; j++) e->name[j] = (uint8_t)mn[j];
            e->attributes = 0x10;   /* 目录 */
            total++;
        }
        return total;
    }
    return n;
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
    if (!current_task || current_task->euid != 0) return -1;   /* 仅 root 可挂载 */
    if (!dev_name || !point) return -1;
    if (!vmm_is_user_accessible(dev_name) || !vmm_is_user_accessible(point)) return -1;
    return vfs_mount((const char*)point, (const char*)dev_name) ? 0 : -1;
}

/* 卸载（ebx=挂载点） */
static int sys_umount(uint32_t point) {
    if (!current_task || current_task->euid != 0) return -1;   /* 仅 root 可卸载 */
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

        case SYS_CURSOR:
            /* ebx = 0 隐藏 / 1 显示 */
            ret = sys_cursor_visible(regs->ebx);
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

        case SYS_READ_FILE_OFF:
            /* ebx=文件名, ecx=偏移, edx=缓冲, esi=最大长度 */
            ret = sys_read_file_off(regs->ebx, regs->ecx, regs->edx, regs->esi);
            break;

        case SYS_GETUID:
            ret = sys_getuid();
            break;

        case SYS_GETGID:
            ret = sys_getgid();
            break;

        case SYS_GETEUID:
            ret = sys_geteuid();
            break;

        case SYS_GETEGID:
            ret = sys_getegid();
            break;

        case SYS_SETUID:
            /* ebx = uid */
            ret = sys_setuid(regs->ebx);
            break;

        case SYS_SETGID:
            /* ebx = gid */
            ret = sys_setgid(regs->ebx);
            break;

        case SYS_STAT:
            /* ebx = 路径, ecx = stat_info_t* */
            ret = sys_stat(regs->ebx, regs->ecx);
            break;

        case SYS_CHMOD:
            /* ebx = 路径, ecx = mode */
            ret = sys_chmod(regs->ebx, regs->ecx);
            break;

        case SYS_CHOWN:
            /* ebx = 路径, ecx = uid, edx = gid */
            ret = sys_chown(regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_POWEROFF:
            /* 关机：不返回 */
            sys_poweroff();
            ret = 0;
            break;

        case SYS_REBOOT:
            /* 重启：不返回 */
            sys_reboot();
            ret = 0;
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
