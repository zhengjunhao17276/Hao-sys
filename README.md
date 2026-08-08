# HaoOS (Hao-sys)

从零写的 i686 裸机操作系统。Multiboot 引导、保护模式、分页、中断、抢占式多任务、FAT 文件系统、PS/2 + USB 驱动、用户态 Shell——没有借助现成内核代码，一路手搓过来的。

## 能干什么

- 完整启动链：Multiboot → 保护模式 → VGA → 物理内存 → 分页 → 进用户态（**启动无日志**，出错才打印）
- 抢占式多任务：PIT 100Hz 时钟，30ms 时间片，内核/用户态任务混排，TSS 切栈
- 驱动：PS/2 键鼠、ATA PIO（主盘 + 从盘）、PCI 扫描、UHCI USB（HID 键盘 + Mass Storage）
- FAT12/16 读写：文件增删、子目录、8.3 短文件名；**文件系统识别**（probe）：FAT/NTFS/exFAT/ext2/3/4/ISO9660/XFS/btrfs 一眼认出，非 FAT 明确拒绝挂载
- **Linux 权限体系**：uid/gid/euid/egid 任务凭据、文件 mode 落盘（FAT 借用字段）、读/写/父目录写权限检查、root 全放行、stat/chmod/chown
- 用户态 Shell：命令历史、行编辑（←→ 移动光标、行中插入）、**鼠标可用**（点击定位光标、TUI 设置界面）、ls -l 长格式
- 电源管理：`shutdown` 关机（QEMU 直接退出）、`reboot` 重启
- 44 个系统调用（int 0x80）

## 构建 & 运行

不需要交叉编译器，系统 gcc 加 `-m32` 就行：

```bash
make -j8 CC="gcc -m32" LD="ld" OBJCOPY="objcopy"
```

**一键构建 + 打包 + 启动：**

```bash
./run.sh
```

`--no-build` 跳过编译只打包启动，`--no-run` 只构建不启动。分步执行也可（手动）:

```bash
./make_boot.sh    # 编译 MBR 引导器（stage1 + stage2）
./make_image.sh   # 打包 HaoOS.img（引导器 + 内核 + FAT16 根文件系统）
```

启动（QEMU）：

```bash
qemu-system-i386 -hda HaoOS.img -display curses
```

也可以把 `HaoOS.img` 写入 U 盘/硬盘在真机上 BIOS 启动。没窗口环境就用 `-display curses`，有窗口就省略。

产物：`kernel.elf`（内核镜像）、`user/shell.bin`（用户态 Shell）、`HaoOS.img`（**单文件可启动镜像**：MBR 引导器 + 内核 + FAT16 根文件系统）、`disk.img`（开发用 FAT16 镜像，`create_disk.sh` 生成，配 `-kernel` 启动）。

## 结构

```
Hao-sys/
├── start/        Multiboot 引导汇编
├── kmain.c       入口和初始化
├── mm/           pmm（位图分配）+ vmm（4KB 分页）
├── syscall/      IDT + int 0x80 分发
├── proc/         任务调度 + 上下文切换
├── driver/       VGA / 键盘 / 鼠标 / ATA / PCI / USB / RTC / 蜂鸣器
├── boot/         MBR 引导器（stage1 + stage2，nasm）
├── fs/           FAT + 挂载 + 文件系统识别（probe.c）
├── lib/          内核字符串库
├── user/         Shell（用户态）
└── include/      头文件
```

启动顺序（kmain.c）：VGA → PMM → VMM → IDT/PIC → PCI → USB → 键鼠 → ATA → FAT → 任务系统 → 加载 SHELL.BIN → iret 进 ring3。全程静默，出错才打印。

## Shell 用法

```
ls [-l] / cat / save / mkdir / rm / cd / pwd    文件操作（ls -l 显示权限长格式）
mount <设备> <挂载点> / umount / devices / usb    挂载与设备（非 FAT 盘明确拒绝）
chmod <mode> <file> / chown <uid> <file> / stat / id / setuid / setgid    权限
settings  设置界面（鼠标调灵敏度/颜色/指针）   busy  忙循环演示抢占
shutdown  关机    reboot  重启
```

`busy` 可以看抢占式调度：纯用户态忙循环会被 PIT 抢走切到 demo 任务。

系统调用约定：`eax=调用号, ebx/ecx/edx=参数, int $0x80, 返回值在 eax`。常用几个：`SYS_PUTCHAR(1)`、`SYS_GETMOUSE(6)`、`SYS_LIST(25)`、`SYS_YIELD(26)`、`SYS_GETKEY_NB(31)`（非阻塞取键，TUI 事件循环用）、`SYS_STAT(44)`、`SYS_POWEROFF(47)`。

## 已知限制

- 内核态不抢占（关中断保护共享状态），只有用户态任务被抢占
- USB 只验证过代码路径——QEMU 没有 UHCI 控制器，真机没测过（DATA toggle、TD 终止位、错误处理还有 4 个待验证点）
- 用户进程共享内核低 4MB 页表，没有进程隔离
- 键盘是轮询模式，等按键时 CPU 忙等（阻塞 getchar 会休眠；getkey_nb 轮询会忙转）
- 非 FAT 文件系统只识别不可读写（probe 认得出 NTFS/ext4 等，但挂载被拒）
- 目录项时间戳未实现（字段借用给 Unix 权限 meta）

## 开发环境

gcc（`-m32 -ffreestanding`）+ nasm + binutils，QEMU 调试。内核有 debugcon（port 0xE9）插桩，配合 `-debugcon stdio` 用。

## License

MIT

## 文档

- [doc/syscalls.md](doc/syscalls.md)——系统调用档案：44 个调用全表，参数/返回值/错误语义/示例
- [doc/shell-developer-guide.md](doc/shell-developer-guide.md)——内核接口档案：写给想写 Shell / 用户态程序的人（加载约定、键盘特殊码、8.3 目录项、构建方式）
