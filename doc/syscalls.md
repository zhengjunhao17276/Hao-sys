# HaoOS 系统调用档案

HaoOS 用户态程序的唯一入口：`int 0x80`。共 42 个调用，覆盖终端输出、键盘鼠标、文件系统、系统信息四类。

## 调用约定

```
eax = 调用号
ebx / ecx / edx = 参数
int $0x80
返回值在 eax
```

- 指针参数会被内核**逐页校验**（`vmm_is_user_accessible`），传非法地址返回 -1，不会崩内核
- 失败统一返回 `-1`，成功返回 `0` 或实际数据（见各条说明）
- 未知调用号：内核打印警告并返回 `-1`（多半是用户程序 bug）

### 内联汇编封装模板

```c
/* 无参，返回 int */
static int sys_noparam(int nr) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(nr) : "memory");
    return r;
}

/* 一参（ebx） */
static int sys_1param(int nr, unsigned int arg1) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(nr), "b"(arg1) : "memory");
    return r;
}

/* 两参（ebx, ecx） */
static int sys_2param(int nr, unsigned int a, unsigned int b) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b) : "memory");
    return r;
}
```

## 一、终端输出

### SYS_PUTCHAR (1)

```
ebx = 字符（低 8 位有效）
返回 0
```

输出单个字符。支持转义：`\n` 换行、`\r` 回车、`\b` 退格（左移+擦除）、`\t` 补到 4 字符边界。受保护区域约束。

### SYS_WRITE (3)

```
ebx = NUL 结尾字符串指针
返回 0 成功 / -1 失败
```

输出整个字符串。限制：
- 最长 4096 字节（内核逐页扫描找 NUL，找不到返回 -1）
- 必须 NUL 结尾，否则静默拒绝

### SYS_SET_CURSOR (7)

```
ebx = 行 (0~24), ecx = 列 (0~79)
返回 0
```

定位光标。越界值会被截断到屏幕边缘，不会出错。受保护区域约束（见 SYS_PROTECT）。

### SYS_GET_CURSOR (8)

```
返回 (行 << 16) | 列
```

读当前光标位置。

### SYS_CLEAR (9)

```
返回 0
```

清屏，光标回 (0,0)，**同时清除保护区域**。

### SYS_GET_COLOR (12) / SYS_SET_COLOR (13)

```
GET:  返回当前默认颜色属性字节
SET:  ebx = 属性字节（如 0x0F = 黑底白字），返回 0
```

属性字节 = 高 4 位背景色，低 4 位前景色。标准 VGA 色号：0 黑 1 蓝 2 绿 3 青 4 红 5 品红 6 棕 7 白。

### SYS_PROTECT (14)

```
返回 0
```

把**当前光标位置**设为保护边界。此后保护区域（边界之前的行/列）不可被写入、光标不可移入。Shell 每打印完提示符调用一次，把编辑区限制在提示行以下——历史输出不会再被覆盖。

## 二、键盘 / 鼠标

### SYS_GETCHAR (2)

```
返回键值（阻塞）
```

**阻塞**取键。无键时任务休眠（`sti;hlt`），等 IRQ1 唤醒，不烧 CPU。适合纯键盘交互的程序。

### SYS_GETKEY_NB (31)

```
返回键值 / -1（无键）
```

**非阻塞**取键，无键立即返回 -1。适合 TUI 事件循环：每轮先取键，无键再去轮询鼠标。⚠️ 键盘 IRQ 被屏蔽（纯轮询模式），内核会直接查 0x64/0x60 端口。

### SYS_GETUID / SYS_GETGID / SYS_GETEUID / SYS_GETEGID (38-41)

```
返回 uid / gid / euid / egid
```

**任务凭据读取**：返回当前任务（即发起调用的进程）的 Unix 式用户/组 ID。新任务零初始化 → 默认 root(0)。

### SYS_SETUID / SYS_SETGID (42-43)

```
返回 0 / -1
```

**切换用户/组**：root(euid=0) 可任意设置（真实+生效 ID 一起改）；非 root 只能把 euid 设回自己的真实 uid（Linux 语义，非 root 无法升权回 root）。

### SYS_STAT (44)

```
ebx=路径, ecx=stat_info_t*（16 字节：mode/uid/gid/size），返回 0/-1
```

**取文件元数据**：mode 含类型位（S_IFDIR/S_IFREG）+ rwxrwxrwx + suid/sgid/sticky；旧文件（无权限元数据）返回默认值（目录 0755、文件 0644，root 属主）。

### SYS_CHMOD (45)

```
ebx=路径, ecx=mode（八进制权限位），返回 0/-1
```

**改权限**：仅属主或 root；只改权限位，类型位保持。

### SYS_CHOWN (46)

```
ebx=路径, ecx=uid, edx=gid（0xFFFF=保持原值），返回 0/-1
```

**改属主/组**：仅 root。

### 权限体系说明

- 权限检查（mask：4=读 2=写 1=执行）：euid==属主 → 属主位，egid==组 → 组位，否则其他位；**root 全放行**
- 读文件/列目录：需文件/目录读权限；写文件（覆盖）/删文件/建目录：需**父目录写权限**（Linux 语义）；挂载/卸载：仅 root
- Unix meta 存于 FAT 目录项借用字段（时间字段当前未实现）：offset13/14=mode，offset15=魔数 0x58，16-17=uid，18-19=gid；无魔数 → 默认 meta
- 新建文件默认 0644、新目录默认 0755，属主=当前任务 uid

### SYS_CURSOR (37)

```
返回 0
```

**光标可见性**：ebx=0 隐藏硬件光标、1=显示。全屏 TUI（vi 编辑器等）进入时隐藏、退出时恢复，消除硬件光标闪烁干扰。

- ebx = 0（隐藏）或 1（显示）
- 对应内核 `vga_disable_cursor()` / `vga_enable_cursor()`（CRTC 0x0A bit5）

### 方向键特殊码

内核把四个方向键转成与 ASCII 控制码不冲突的特殊码返回（**不会**产生 ANSI 序列）：

| 键 | 码 | 语义 |
|---|---|---|
| ↑ | 0x01 | 历史上翻 |
| ↓ | 0x02 | 历史下翻 |
| ← | 0x03 | 光标左移 |
| → | 0x04 | 光标右移 |

### SYS_GETMOUSE (6)

```
ebx = 用户缓冲（3 × int32）
返回 0 成功 / -1 失败
```

写入 `[x, y, buttons]`：
- x: 0~79（列），y: 0~24（行），与屏幕坐标一致
- buttons: bit0 左键、bit1 右键、bit2 中键
- 鼠标不可用时返回全 0

**点击检测**（内核只上报状态，不报事件）：记录左键按下时的 (x,y)，抬起时若位置未变才算一次点击；按住拖动不算。

### SYS_GET_SENS (10) / SYS_SET_SENS (11)

```
GET: 返回灵敏度 (1~16)
SET: ebx = 灵敏度 (1~16)，返回 0
```

鼠标灵敏度。默认 8。

### SYS_GET_PGLYPH (15) / SYS_SET_PGLYPH (16)

```
GET: 返回当前指针图案字形码
SET: ebx = CP437 字形码（如 0xDB = █），返回 0
```

鼠标指针图案。指针由内核绘制（反显/字形），程序输出前无需擦除。

## 三、文件系统

路径格式：`/foo/bar.bin`，前缀路由到挂载点（`/` = 内置盘，`/usb` = USB 盘）。文件名只支持 **8.3 短名**（大写，不足补空格）。

### SYS_WRITE_FILE (17)

```
ebx = 文件名, ecx = 数据指针, edx = 长度
返回 0 成功 / -1 失败
```

写文件。限制：数据 ≤ 64KB。已存在则覆盖。

### SYS_READ_FILE (18)

```
ebx = 文件名, ecx = 缓冲, edx = 最大长度
返回实际字节数 / -1 失败
```

读整个文件进缓冲。

### SYS_DELETE_FILE (22)

```
ebx = 文件名
返回 0 成功 / -1 失败
```

删除文件或**空**目录。

### SYS_MKDIR (24)

```
ebx = 路径
返回 0 成功 / -1 失败
```

创建子目录。

### SYS_LIST (25)

```
ebx = 路径（NULL = 根目录）, ecx = fat_dirent_t 数组, edx = 最大项数
返回实际项数 / -1 失败
```

列目录。目录项为 32 字节 `fat_dirent_t`，布局：

```c
struct dirent {
    unsigned char name[11];     /* 8.3：8 文件名 + 3 扩展名，右补空格 */
    unsigned char attributes;   /* bit4 (0x10) = 目录 */
    unsigned char nt_reserved;
    unsigned char creation_time_tenths;
    unsigned short creation_time;
    unsigned short creation_date;
    unsigned short last_access_date;
    unsigned short cluster_high;
    unsigned short last_write_time;
    unsigned short last_write_date;
    unsigned short cluster_low;
    unsigned int file_size;
};
```

注意：name[11] 是**带空格填充**的原始 8.3 名（如 `"HELLO   TXT"`），需要自己转可读名（去尾空格，扩展名加点）。首字节 0x00 = 目录结束，0xE5 = 已删除项。

### SYS_MOUNT (28) / SYS_UMOUNT (29)

```
MOUNT:  ebx = 设备名, ecx = 挂载点，返回 0 / -1
UMOUNT: ebx = 挂载点，返回 0 / -1
```

挂载/卸载块设备（如 `mount usb0 /usb`）。

## 四、系统信息 / 任务

### SYS_EXIT (4)

```
ebx = 状态码
不返回
```

终止当前任务，交给调度器清理（摘链表、释放 PCB/内核栈）。

### SYS_READ_SECT (5)

```
ebx = LBA, ecx = 512B 缓冲
返回 0 成功 / 1 失败
```

裸读磁盘扇区（演示用）。

### SYS_TASKS (19)

```
返回 0
```

内核向终端打印任务列表。

### SYS_GET_TIME (20)

```
返回 (时 << 16) | (分 << 8) | 秒
```

### SYS_GET_DATE (21)

```
返回 ((年-2000) << 16) | (月 << 8) | 日
```

### SYS_UPTIME (23)

```
返回开机秒数
```

### SYS_YIELD (26)

```
返回值不可用，忽略
```

主动让出 CPU。协作式调度下任务不主动让就没得切换；配抢占式调度（PIT 100Hz）用于 TUI 事件循环里避免忙转。

### SYS_USB_INFO (27) / SYS_DEVICES (30)

```
返回 0
```

内核向终端打印 USB 设备列表 / 设备与挂载表（调试用）。

## 快速索引

| 号 | 名称 | 一句话 |
|---|---|---|
| 1 | SYS_PUTCHAR | 输出单字符 |
| 2 | SYS_GETCHAR | 阻塞取键 |
| 3 | SYS_WRITE | 输出字符串（≤4096B，NUL 结尾） |
| 4 | SYS_EXIT | 终止任务 |
| 5 | SYS_READ_SECT | 裸读扇区 |
| 6 | SYS_GETMOUSE | 读鼠标 (x, y, buttons) |
| 7 | SYS_SET_CURSOR | 定位光标 |
| 8 | SYS_GET_CURSOR | 读光标 (行<<16)\|列 |
| 9 | SYS_CLEAR | 清屏 |
| 10 | SYS_GET_SENS | 读鼠标灵敏度 |
| 11 | SYS_SET_SENS | 写鼠标灵敏度 1~16 |
| 12 | SYS_GET_COLOR | 读默认颜色 |
| 13 | SYS_SET_COLOR | 写默认颜色 |
| 14 | SYS_PROTECT | 设保护边界（光标前只读） |
| 15 | SYS_GET_PGLYPH | 读指针图案 |
| 16 | SYS_SET_PGLYPH | 写指针图案 |
| 17 | SYS_WRITE_FILE | 写文件（≤64KB） |
| 18 | SYS_READ_FILE | 读文件 |
| 19 | SYS_TASKS | 打印任务列表 |
| 20 | SYS_GET_TIME | 读时间 |
| 21 | SYS_GET_DATE | 读日期 |
| 22 | SYS_DELETE_FILE | 删文件/空目录 |
| 23 | SYS_UPTIME | 开机秒数 |
| 24 | SYS_MKDIR | 建目录 |
| 25 | SYS_LIST | 列目录 |
| 26 | SYS_YIELD | 让出 CPU |
| 27 | SYS_USB_INFO | 打印 USB 列表 |
| 28 | SYS_MOUNT | 挂载设备 |
| 29 | SYS_UMOUNT | 卸载 |
| 30 | SYS_DEVICES | 打印设备/挂载表 |
| 31 | SYS_GETKEY_NB | 非阻塞取键 |
| 36 | SYS_READ_FILE_OFF | 偏移读文件 |
| 37 | SYS_CURSOR | 光标显隐 |
| 38 | SYS_GETUID | 读真实 uid |
| 39 | SYS_GETGID | 读真实 gid |
| 40 | SYS_GETEUID | 读生效 uid |
| 41 | SYS_GETEGID | 读生效 gid |
| 42 | SYS_SETUID | 设 uid |
| 43 | SYS_SETGID | 设 gid |
| 44 | SYS_STAT | 取文件元数据 |
| 45 | SYS_CHMOD | 改权限 |
| 46 | SYS_CHOWN | 改属主 |

## 与 Shell 指南的关系

- 本文档：纯接口参考（每个调用的参数/返回/坑）
- [shell-developer-guide.md](shell-developer-guide.md)：写 Shell 的完整教程（加载方式、目录项详解、建议结构、构建命令），引用本文档的调用表
