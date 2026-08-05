#!/usr/bin/env bash
# ============================================================
# install-toolchain.sh - 为 Hao-sys 安装构建工具链（Ubuntu/Debian）
#   安装: nasm（汇编器）+ qemu-system-x86（模拟器）
#         + i686-elf 交叉编译器（预编译 GCC 15.2.0，约 874MB）
# 用法: bash install-toolchain.sh
# ============================================================
set -euo pipefail

echo "==> [1/4] 安装系统包: nasm / qemu / 基础工具"
sudo apt update
sudo apt install -y nasm qemu-system-x86 build-essential unzip wget

echo "==> [2/4] 下载 i686-elf 交叉编译器 (GCC 15.2.0)"
mkdir -p "$HOME/opt"
cd "$HOME/opt"
if ! find "$HOME/opt" -name i686-elf-gcc -type f 2>/dev/null | grep -q .; then
    wget -c https://github.com/lordmilko/i686-elf-tools/releases/download/15.2.0/i686-elf-tools-linux.zip
    unzip -q i686-elf-tools-linux.zip
    rm -f i686-elf-tools-linux.zip
else
    echo "    已检测到 i686-elf-gcc，跳过下载"
fi

echo "==> [3/4] 写入 PATH 到 ~/.bashrc"
TOOLCHAIN_BIN="$(dirname "$(find "$HOME/opt" -name i686-elf-gcc -type f | head -1)")"
if ! grep -q "i686-elf-tools" "$HOME/.bashrc" 2>/dev/null; then
    echo "export PATH=\"$TOOLCHAIN_BIN:\$PATH\"" >> "$HOME/.bashrc"
fi
export PATH="$TOOLCHAIN_BIN:$PATH"

echo "==> [4/4] 验证"
i686-elf-gcc --version | head -1
nasm -v | head -1
qemu-system-i386 --version | head -1

echo ""
echo "完成！新开的终端会自动带上 PATH；当前终端继续用的话先执行:"
echo "  export PATH=\"$TOOLCHAIN_BIN:\$PATH\""
echo "然后到 Hao-sys 目录: make && make run"
