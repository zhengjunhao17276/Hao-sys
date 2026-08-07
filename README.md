# HaoOS (Hao-sys)

从零写的 i686 裸机操作系统。Multiboot 引导、保护模式、分页、中断、抢占式多任务、FAT 文件系统、PS/2 + USB 驱动、用户态 Shell——没有借助现成内核代码，一路手搓过来的。

## 能干什么

- 完整启动链：Multiboot → 保护模式 → VGA → 物理内存 → 分页 → 进用户态
- 抢占式多任务：PIT 100Hz 时钟，30ms 时间片，内核/用户态任务混排，TSS 切栈
- 驱动：PS/2 键鼠、ATA PIO、PCI 扫描、UHCI USB（HID 键盘 + Mass Storage）
- FAT12/16 读写：文件增删、子目录、8.3 短文件名
- 用户态 Shell：命令历史、行编辑（←→ 移动光标、行中插入）、**鼠标可用**（点击定位光标、TUI 设置界面）
- 31 个系统调用（int 0x80）

## 构建 & 运行

不需要交叉编译器，系统 gcc 加 `-m32` 就行：

```bash
make -j8 CC="gcc -m32" LD="ld" OBJCOPY="objcopy"
```

跑起来（QEMU）：

```bash
qemu-system-i386 -kernel kernel.elf -hda disk.img -display curses -monitor none
```

没窗口环境就用 `-display curses`，有窗口就省略它。

产物：`kernel.elf`（内核镜像）、`user/shell.bin`（用户态 Shell，写进磁盘镜像）、`disk.img`（32MB FAT16 镜像，`create_disk.sh` 生成）。

## 结构

```
Hao-sys/
├── start/        Multiboot 引导汇编
├── kmain.c       入口和初始化
├── mm/           pmm（位图分配）+ vmm（4KB 分页）
├── syscall/      IDT + int 0x80 分发
├── proc/         任务调度 + 上下文切换
├── driver/       VGA / 键盘 / 鼠标 / ATA / PCI / USB / RTC / 蜂鸣器
├── fs/           FAT12/16 + 挂载
├── lib/          内核字符串库
├── user/         Shell（用户态）
└── include/      头文件
```

启动顺序（kmain.c）：VGA → PMM → VMM → IDT/PIC → PCI → USB → 键鼠 → ATA → FAT → 任务系统 → 加载 SHELL.BIN → iret 进 ring3。

## Shell 用法

```
help     帮助        clear    清屏        tasks    任务列表
time     日期时间    uptime   开机时长
ls/mkdir/rm/cat/save        FAT 文件操作
settings 设置界面（鼠标调灵敏度/颜色/指针）  exit  退出
```

`busy` 可以看抢占式调度：纯用户态忙循环会被 PIT 抢走切到 demo 任务。

系统调用约定：`eax=调用号, ebx/ecx/edx=参数, int $0x80, 返回值在 eax`。常用几个：`SYS_PUTCHAR(1)`、`SYS_GETMOUSE(6)`、`SYS_LIST(25)`、`SYS_YIELD(26)`、`SYS_GETKEY_NB(31)`（非阻塞取键，TUI 事件循环用）。

## 已知限制

- 内核态不抢占（关中断保护共享状态），只有用户态任务被抢占
- USB 只验证过代码路径——QEMU 没有 UHCI 控制器，真机没测过（DATA toggle、TD 终止位、错误处理还有 4 个待验证点）
- 用户进程共享内核低 4MB 页表，没有进程隔离
- 键盘是轮询模式，等按键时 CPU 忙等

## 开发环境

gcc（`-m32 -ffreestanding`）+ nasm + binutils，QEMU 调试。内核有 debugcon（port 0xE9）插桩，配合 `-debugcon stdio` 用。

## License

MIT

## 文档

- [doc/shell-developer-guide.md](doc/shell-developer-guide.md)——内核接口档案：写给想写 Shell / 用户态程序的人（31 个 syscall 全表、加载约定、键盘特殊码、8.3 目录项、构建方式）
