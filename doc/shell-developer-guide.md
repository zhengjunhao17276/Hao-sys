# HaoOS 内核接口档案——写给 Shell 开发者

本文档说明如何为 HaoOS 编写用户态程序（重点是 Shell）。所有 I/O 都通过 `int 0x80` 系统调用完成，运行在 ring 3，不能直接访问硬件和显存。

## 1. 程序如何被加载

内核启动时从 FAT 根目录找 `SHELL.BIN`，加载到固定虚拟地址后 `iret` 进用户态：

| 项目 | 值 |
|---|---|
| 加载路径 | `/SHELL.BIN`（磁盘根目录） |
| 代码虚拟地址 | `0x10000000` |
| 栈 | 代码页之后，`0x10007000` 起（含 1 个 guard page） |
| 入口 | 文件开头第一个字节（扁平二进制，非 ELF） |
| 大小限制 | 256KB |

```c
/* 引导入口：跳到 main（必须放在文件最前） */
__attribute__((naked)) void _entry(void) {
    __asm__ volatile ("jmp main");
}
```

## 2. 系统调用约定

```
eax = 调用号
ebx / ecx / edx = 参数
int $0x80
返回值在 eax
```

内联汇编封装示例：

```c
static void putchar(char c) {
    __asm__ volatile ("int $0x80" : : "a"(1), "b"((unsigned int)c) : "memory");
}

static int getmouse(int *x, int *y, int *buttons) {
    unsigned int buf[3];
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(6), "b"((unsigned int)buf) : "memory");
    if (ret == 0) {
        *x = (int)buf[0];
        *y = (int)buf[1];
        *buttons = (int)buf[2];
    }
    return ret;
}
```

> 指针参数会被内核逐页校验（`vmm_is_user_accessible`），必须指向用户可访问内存，不能传内核地址或野指针。

## 3. 系统调用完整列表（31 个）

完整档案见 [syscalls.md](syscalls.md)——每个调用的参数、返回值、错误语义、示例都有。这里只列分类速查表：

| 类 | 调用 |
|---|---|
| 终端输出 | PUTCHAR(1) WRITE(3) SET_CURSOR(7) GET_CURSOR(8) CLEAR(9) GET_COLOR(12) SET_COLOR(13) PROTECT(14) |
| 键盘鼠标 | GETCHAR(2) GETKEY_NB(31) GETMOUSE(6) GET_SENS(10) SET_SENS(11) GET_PGLYPH(15) SET_PGLYPH(16) |
| 文件系统 | WRITE_FILE(17) READ_FILE(18) DELETE_FILE(22) MKDIR(24) LIST(25) MOUNT(28) UMOUNT(29) |
| 系统信息 | EXIT(4) READ_SECT(5) TASKS(19) GET_TIME(20) GET_DATE(21) UPTIME(23) YIELD(26) USB_INFO(27) DEVICES(30) |

调用约定：`eax=调用号, ebx/ecx/edx=参数, int $0x80, 返回值在 eax`。内联汇编封装示例：

```c
static void putchar(char c) {
    __asm__ volatile ("int $0x80" : : "a"(1), "b"((unsigned int)c) : "memory");
}

static int getmouse(int *x, int *y, int *buttons) {
    unsigned int buf[3];
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(6), "b"((unsigned int)buf) : "memory");
    if (ret == 0) {
        *x = (int)buf[0];
        *y = (int)buf[1];
        *buttons = (int)buf[2];
    }
    return ret;
}
```

> 指针参数会被内核逐页校验（`vmm_is_user_accessible`），必须指向用户可访问内存，不能传内核地址或野指针。

## 4. 键盘特殊码（方向键）

内核把方向键转成与 ASCII 控制码不冲突的特殊码，**不会**产生 `ESC [ A` 这类 ANSI 序列：

| 键 | 码 | 语义 |
|---|---|---|
| ↑ | 0x01 | 历史上翻 |
| ↓ | 0x02 | 历史下翻 |
| ← | 0x03 | 光标左移 |
| → | 0x04 | 光标右移 |

## 5. 目录项结构（SYS_LIST）

`fat_dirent_t`，32 字节，与内核 `include/fs/fat.h` 布局一致：

```c
struct dirent {
    unsigned char name[11];     /* 8.3 格式：8 文件名 + 3 扩展名，不足右补空格 */
    unsigned char attributes;   /* bit4=目录（0x10） */
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

8.3 名转可读名：基本名去尾空格，扩展名去尾空格后加 `.` 拼上。首字节 `0x00`=目录结束，`0xE5`=已删除项。

## 6. 屏幕与鼠标坐标

- 文本模式 **80×25**（VGA），坐标 (row, col)，row 0~24，col 0~79
- 鼠标坐标与屏幕一致：x=0~79, y=0~24；**指针由内核绘制**（反显），程序输出前无需擦除
- 鼠标点击需要自己检测边沿：记录左键按下时的 (x,y)，抬起时若位置未变才算一次点击（按住拖动不算）
- `SYS_PROTECT` 配合行编辑：每打印完提示符调用一次，把编辑区限制在提示行以下，防止点击/光标误入历史区

## 7. 建议的 Shell 结构

```c
while (1) {
    write("$ ");
    protect();                      /* 锁定提示行以下为编辑区 */
    len = readline(line, 128);      /* 行编辑：非阻塞取键 + 鼠标点击定位 */
    execute(line);                  /* 查命令表执行 */
    yield();                        /* 让出 CPU */
}
```

- **阻塞**程序（纯键盘交互）用 `SYS_GETCHAR`（休眠等键，不烧 CPU）
- **TUI/事件循环**程序（要响应鼠标）用 `SYS_GETKEY_NB` 轮询 + 无事件时 `SYS_YIELD`（抢占式调度会按时唤醒，不会忙转）
- 超宽输入（>80 列）：编辑行触底前光标跳底行发 `\n` 触发内核滚动，`start_row` 同步减（参考 `shell.c` 的 `ensure_edit_space`）

## 8. 构建

```bash
# 用户态程序编译成扁平二进制（无 libc，-ffreestanding）
gcc -m32 -ffreestanding -fno-pic -fno-stack-protector -c shell.c -o shell.o
ld -m elf_i386 -Ttext 0 -e _entry --oformat binary shell.o -o shell.bin
```

`create_disk.sh` 会把 `user/shell.bin` 写进 `disk.img` 的 FAT 根目录。换 Shell 后重新生成磁盘镜像再启动 QEMU。

> 注意：内核当前只自动加载 `SHELL.BIN` 这一个用户程序；想跑多个程序需扩展 `kmain.c` 的加载逻辑（加载地址、页表映射、任务创建）。
