# HaoOS (Hao-sys)

一个从零编写的 **i686 裸机操作系统内核**——Multiboot 引导、保护模式、分页、中断、多任务、FAT16 文件系统、PS/2/USB 驱动、用户态 Shell，全链路亲手实现。

## ✨ 功能特性

- **启动链**：Multiboot → 保护模式 → VGA 终端 → 物理内存管理 → 分页
- **内存管理**：位图式物理内存分配器（PMM）+ 4KB 分页虚拟内存（VMM，身份映射）
- **中断**：IDT 256 门 + 8259A PIC 重映射，异常/IRQ/系统调用（int 0x80）全链路
- **多任务**：**抢占式**时间片轮转（PIT 100Hz 时钟源，30ms 时间片），内核/用户态任务混合调度，TSS 内核栈切换，完整寄存器上下文恢复
- **设备驱动**：PS/2 键盘/鼠标、ATA PIO、PCI 扫描、UHCI USB（HID 键盘 + Mass Storage BOT）
- **文件系统**：FAT12/16 读写（创建/删除文件、目录、长文件名 8.3 格式）
- **用户态**：Shell（命令历史、TUI 设置界面）+ 27 个系统调用
- **演示**：多任务 demo 内核任务，与 Shell 轮转运行

## 🚀 快速开始

### 构建（无需交叉编译器！）

系统 gcc 的 `-m32` 模式即可构建，**不需要**下载 i686-elf 交叉工具链（874MB）：

```bash
make -j8 CC="gcc -m32" LD="ld" OBJCOPY="objcopy"
```

> 旧方案 `install-toolchain.sh`（下载 i686-elf-tools）仍可用，但已无必要。

### 运行（QEMU）

```bash
qemu-system-i386 -kernel kernel.elf -hda disk.img -display curses -monitor none
```

- `-display curses`：在终端里显示 VGA 文本（无窗口环境友好）
- 或去掉 `-display curses` 用默认图形窗口

### 构建产物

| 产物 | 说明 |
|---|---|
| `kernel.elf` | Multiboot 内核镜像 |
| `user/shell.bin` | 用户态 Shell（FAT16 格式写入磁盘镜像） |
| `disk.img` | 32MB FAT16 磁盘镜像（含 SHELL.BIN，`create_disk.sh` 生成） |

## 🏗️ 架构

```
┌─────────────────────────────────────────────────┐
│  用户态 (ring 3)                                 │
│  Shell ── int 0x80 ──→ 27 个系统调用            │
├─────────────────────────────────────────────────┤
│  内核态 (ring 0)                                 │
│  ├─ 任务调度器（协作式轮转，TSS esp0 切换）      │
│  ├─ 文件系统：FAT12/16（ATA PIO 读写）          │
│  ├─ 驱动：PS/2 键鼠 / PCI / UHCI USB            │
│  ├─ 虚拟内存：4KB 分页（身份映射）              │
│  └─ 物理内存：位图分配器 + 中断系统（IDT/PIC）  │
└─────────────────────────────────────────────────┘
```

### 启动顺序（`kmain.c`）

```
VGA → PMM（物理内存）→ VMM（分页）→ IDT/PIC（中断）
→ PCI → USB → 键盘/鼠标 → ATA → FAT → 任务系统 → 加载 SHELL.BIN → iret 进用户态
```

### 目录结构

```
Hao-sys/
├── start/        Multiboot 引导汇编
├── kmain.c       内核入口与初始化
├── mm/           物理内存 (pmm) + 虚拟内存 (vmm)
├── syscall/      中断描述符表 (idt) + 系统调用 (syscall)
├── proc/         任务管理 (task) + 上下文切换汇编 (switch)
├── driver/       VGA/键盘/鼠标/ATA/PCI/USB/RTC/蜂鸣器
├── fs/           FAT12/16 文件系统
├── lib/          内核字符串库
├── user/         用户态 Shell
└── include/      全部头文件
```

## 💻 Shell 命令

```
help      帮助    echo   回显    clear   清屏
tasks     任务列表  time   日期时间  uptime  开机时长
mkdir/ls/rm/cat/save     FAT 文件操作
settings  TUI 设置界面（鼠标可用）   exit    退出 Shell
```

## 📋 系统调用（部分）

`SYS_PUTCHAR(1)` `SYS_GETCHAR(2)` `SYS_WRITE(3)` `SYS_EXIT(4)` `SYS_READ_SECT(5)`
`SYS_GETMOUSE(6)` `SYS_WRITE_FILE(17)` `SYS_READ_FILE(18)` `SYS_TASKS(19)`
`SYS_GET_TIME(20)` `SYS_GET_DATE(21)` `SYS_UPTIME(23)` `SYS_MKDIR(24)` `SYS_LIST(25)` `SYS_YIELD(26)`

> 提示：输入 `busy` 可观察抢占式调度——纯用户态忙循环期间会被 PIT 抢占切到 demo 任务。

调用约定：`eax=调用号, ebx/ecx/edx=参数, int $0x80, 返回值在 eax`。

## 🐛 已知限制

- 只在用户态抢占（内核态关中断，避免共享状态重入）；内核态抢占需要可重入改造
- USB 驱动仅验证了代码路径（QEMU 无 UHCI 控制器），真机待测
  （DATA toggle 跟踪、TD 终止位、错误处理等 4 个待验证点）
- 用户进程共享内核低 4MB 页表，进程空间隔离未实现
- 键盘为轮询模式（等待按键时 CPU 忙等，不依赖 IRQ）

## 🛠️ 开发环境

- 构建：gcc（`-m32 -ffreestanding`）+ nasm + binutils
- 运行：QEMU（`qemu-system-i386`）
- 调试：QEMU `-debugcon stdio` + Bochs 调试口（port 0xE9）插桩

## License

MIT
