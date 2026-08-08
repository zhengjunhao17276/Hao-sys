#!/usr/bin/env bash
# make_boot.sh - 编译 MBR 引导器（stage1 + stage2）
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_LEN=$(stat -c %s "$DIR/kernel.elf")
KERNEL_SECTORS=$(( (KERNEL_LEN + 511) / 512 ))
nasm -f bin "$DIR/boot/stage1.asm" -o "$DIR/boot/stage1.bin"
nasm -f bin -DKERNEL_LBA=3 -DKERNEL_SECTORS=$KERNEL_SECTORS "$DIR/boot/stage2.asm" -o "$DIR/boot/stage2.bin"
echo "stage1.bin: $(stat -c %s "$DIR/boot/stage1.bin") B, stage2.bin: $(stat -c %s "$DIR/boot/stage2.bin") B (kernel=$KERNEL_SECTORS sectors)"
