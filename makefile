# ============================================================
# Makefile - 完整构建脚本（Linux 移植版）
# ============================================================

CC      = i686-elf-gcc
LD      = i686-elf-ld
ASM     = nasm
OBJCOPY = i686-elf-objcopy

CFLAGS   = -std=c11 -ffreestanding -nostdlib -fno-stack-protector \
           -Iinclude -Wall -Wextra -fno-leading-underscore \
           -march=i386 -mtune=generic -MMD -MP
ASMFLAGS = -f elf32
LDFLAGS  = -T linker.ld -m elf_i386

OBJS = start.o isr_asm.o kmain.o vga.o pic.o keyboard.o mouse.o speaker.o \
       ata.o pci.o usb_controller.o usb_hid.o usb_mass_storage.o \
       idt.o syscall.o pmm.o vmm.o task.o switch.o user_enter.o string.o fat.o vfs.o rtc.o pit.o probe.o

SHELL_SRC  = user/shell.c
SHELL_OBJ  = user/shell.o
SHELL_ELF  = user/shell.elf
SHELL_BIN  = user/shell.bin
DISK_IMG   = disk.img

all: kernel.elf $(SHELL_BIN) $(DISK_IMG)

# ---------- 内核规则 ----------
start.o: start/start.asm
	$(ASM) $(ASMFLAGS) $< -o $@

isr_asm.o: syscall/isr_asm.asm
	$(ASM) $(ASMFLAGS) $< -o $@

kmain.o: kmain.c
	$(CC) $(CFLAGS) -c $< -o $@

vga.o: driver/vga.c
	$(CC) $(CFLAGS) -c $< -o $@

pic.o: driver/pic.c
	$(CC) $(CFLAGS) -c $< -o $@

keyboard.o: driver/keyboard.c
	$(CC) $(CFLAGS) -c $< -o $@

mouse.o: driver/mouse.c
	$(CC) $(CFLAGS) -c $< -o $@

speaker.o: driver/speaker.c
	$(CC) $(CFLAGS) -c $< -o $@

ata.o: driver/ata.c
	$(CC) $(CFLAGS) -c $< -o $@

pci.o: driver/pci.c
	$(CC) $(CFLAGS) -c $< -o $@

usb_controller.o: driver/usb_controller.c
	$(CC) $(CFLAGS) -c $< -o $@

usb_hid.o: driver/usb_hid.c
	$(CC) $(CFLAGS) -c $< -o $@

usb_mass_storage.o: driver/usb_mass_storage.c
	$(CC) $(CFLAGS) -c $< -o $@

idt.o: syscall/idt.c
	$(CC) $(CFLAGS) -c $< -o $@

syscall.o: syscall/syscall.c
	$(CC) $(CFLAGS) -c $< -o $@

pmm.o: mm/pmm.c
	$(CC) $(CFLAGS) -c $< -o $@

vmm.o: mm/vmm.c
	$(CC) $(CFLAGS) -c $< -o $@

task.o: proc/task.c
	$(CC) $(CFLAGS) -c $< -o $@

switch.o: proc/switch.asm
	$(ASM) $(ASMFLAGS) $< -o $@

user_enter.o: proc/user_enter.asm
	$(ASM) $(ASMFLAGS) $< -o $@

# 嵌入 Shell 二进制（.bin -> .o）
user/shell_embedded.o: user/shell_minimal.bin
	$(LD) -r -b binary -o $@ $<

string.o: lib/string.c
	$(CC) $(CFLAGS) -c $< -o $@

fat.o: fs/fat.c
	$(CC) $(CFLAGS) -c $< -o $@

probe.o: fs/probe.c
	$(CC) $(CFLAGS) -c $< -o $@

vfs.o: fs/vfs.c
	$(CC) $(CFLAGS) -c $< -o $@

rtc.o: driver/rtc.c
	$(CC) $(CFLAGS) -c $< -o $@

pit.o: driver/pit.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# ---------- 用户 Shell ----------
# 先编译 .o，再链接 .elf，最后 objcopy 为纯二进制
$(SHELL_OBJ): $(SHELL_SRC)
	$(CC) -m32 -ffreestanding -nostdlib -fno-stack-protector -mno-red-zone -O0 -MMD -MP -c $< -o $@

$(SHELL_ELF): $(SHELL_OBJ)
	$(LD) -m elf_i386 -Ttext 0x10000000 -e main -o $@ $<

$(SHELL_BIN): $(SHELL_ELF)
	$(OBJCOPY) -O binary $< $@

# ---------- 磁盘镜像 ----------
# Linux：create_disk.sh（bash 移植版，仅 coreutils）
# Windows：create_disk.ps1（PowerShell 原版）
ifeq ($(OS),Windows_NT)
DISK_CMD = powershell -ExecutionPolicy Bypass -File create_disk.ps1
else
DISK_CMD = bash create_disk.sh
endif

$(DISK_IMG): $(SHELL_BIN) create_disk.sh
	$(DISK_CMD)

# ---------- 运行 ----------
run: kernel.elf $(DISK_IMG)
	@echo ""
	qemu-system-i386 -kernel kernel.elf -hda "$(CURDIR)/$(DISK_IMG)"

run-atthisconsole: kernel.elf $(DISK_IMG)
	qemu-system-i386 -kernel kernel.elf -hda "$(CURDIR)/$(DISK_IMG)" -display curses

# ---------- 清理 ----------
clean:
	rm -f *.o *.elf *.bin *.d
	rm -f driver/*.o lib/*.o mm/*.o proc/*.o syscall/*.o fs/*.o user/*.o
	rm -f driver/*.d lib/*.d mm/*.d proc/*.d syscall/*.d fs/*.d user/*.d
	rm -f user/*.elf user/*.bin
	@echo "[CLEAN] Removed object files, ELF, and binary files. Disk image preserved."

# 完全清理（包括磁盘镜像）
clean-all: clean
	rm -f $(DISK_IMG)
	@echo "[CLEAN] Removed disk image too."

.PHONY: all run run-atthisconsole clean clean-all

# 头文件依赖（-MMD 生成，改 include 头文件自动重编对应 .c）
-include $(OBJS:.o=.d) $(SHELL_OBJ:.o=.d)
