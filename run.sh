#!/usr/bin/env bash
# ============================================================
# run.sh - 一键构建 + 打包 + 启动 HaoOS
#
#   ./run.sh                 # 全流程：make → 引导器 → 镜像 → QEMU
#   ./run.sh --no-build      # 只打包+启动（跳过 make，镜像仍是旧的）
#   ./run.sh --no-run        # 只构建+打包，不启动 QEMU
# ============================================================
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

BUILD=1; RUN=1
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=0 ;;
        --no-run)   RUN=0 ;;
        *) echo "未知参数: $arg" >&2; exit 1 ;;
    esac
done

if [ "$BUILD" = 1 ]; then
    echo "==> 1/3 编译内核 + Shell..."
    make -j8 CC="gcc -m32" LD="ld" OBJCOPY="objcopy" || \
        make -j8 CC="i686-elf-gcc" LD="i686-elf-ld" OBJCOPY="i686-elf-objcopy"
fi

echo "==> 2/3 编译 MBR 引导器 + 打包 HaoOS.img..."
./make_boot.sh
./make_image.sh

if [ "$RUN" = 1 ]; then
    echo "==> 3/3 启动 QEMU（退出 QEMU 即回到本命令）"
    exec qemu-system-i386 -hda HaoOS.img -display curses
fi
