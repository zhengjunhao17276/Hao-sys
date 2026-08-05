#!/usr/bin/env bash
# ============================================================
# create_disk.sh - 创建 disk.img（FAT16，含 SHELL.BIN）
# create_disk.ps1 的 Linux 移植版：仅依赖 coreutils
# （truncate / dd / printf），无需 mkfs.fat 等外部工具
# ============================================================
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHELL_BIN="$DIR/user/shell.bin"
DISK_IMG="$DIR/disk.img"

if [ ! -f "$SHELL_BIN" ]; then
    echo "错误: 找不到 $SHELL_BIN，请先 make user/shell.bin" >&2
    exit 1
fi

SIZE=$((32 * 1024 * 1024))   # 32MB
BPS=512; SPC=2; RES=1; NF=2; REC=512; FAT_SZ=128
TOTAL=$((SIZE / BPS))        # 65536 扇区
SHELL_LEN=$(stat -c %s "$SHELL_BIN")

# ---------- 工具函数（在偏移处写入字节） ----------
write_byte() {  # $1=偏移  $2=值(0-255)
    printf "\\$(printf '%03o' "$2")" | dd of="$DISK_IMG" bs=1 seek="$1" conv=notrunc status=none
}
write_le16() {  # $1=偏移  $2=值
    local o=$1 v=$2
    write_byte "$o"       $((v & 0xFF))
    write_byte "$((o+1))" $(((v >> 8) & 0xFF))
}
write_le32() {  # $1=偏移  $2=值
    local o=$1 v=$2
    write_byte "$o"       $((v & 0xFF))
    write_byte "$((o+1))" $(((v >> 8) & 0xFF))
    write_byte "$((o+2))" $(((v >> 16) & 0xFF))
    write_byte "$((o+3))" $(((v >> 24) & 0xFF))
}
write_ascii() { # $1=偏移  $2=字符串（不含 NUL）
    printf '%s' "$2" | dd of="$DISK_IMG" bs=1 seek="$1" conv=notrunc status=none
}

# ---------- 1. 全新空镜像（先删旧文件，避免 truncate 对同尺寸文件留残留数据） ----------
rm -f "$DISK_IMG"
truncate -s "$SIZE" "$DISK_IMG"

# ---------- 2. 引导扇区 + BPB ----------
write_byte 0  0xEB
write_byte 1  0x3C
write_byte 2  0x90
write_ascii 3 "HaoOS   "
write_le16 11 "$BPS"
write_byte 13 "$SPC"
write_le16 14 "$RES"
write_byte 16 "$NF"
write_le16 17 "$REC"
write_le16 19 "$((TOTAL & 0xFFFF))"
write_byte 21 0xF8
write_le16 22 "$FAT_SZ"
write_byte 24 63
write_byte 25 0
write_byte 26 16
write_byte 27 0
write_le32 32 "$TOTAL"
write_byte 36 0x80
write_byte 38 0x29
write_byte 39 0xDE
write_byte 40 0xAD
write_byte 41 0xBE
write_byte 42 0xEF
write_ascii 43 "HAOOS      "
write_ascii 54 "FAT16   "
write_byte 510 0x55
write_byte 511 0xAA

# ---------- 3. FAT #1 ----------
# FAT 从扇区 1 开始；FAT[0]=0xFFF8, FAT[1]=0xFFFF（介质标记）
F1=$((1 * BPS))
write_byte "$F1"       0xF8
write_byte "$((F1+1))" 0xFF
write_byte "$((F1+2))" 0xFF
write_byte "$((F1+3))" 0xFF

# 为 Shell 分配簇链：簇 2 起，最后一簇标记 0xFFF8（链尾）
NC=$(( (SHELL_LEN + SPC*BPS - 1) / (SPC*BPS) ))
for ((i = 0; i < NC; i++)); do
    CL=$((2 + i))
    if [ $i -lt $((NC - 1)) ]; then NXT=$((CL + 1)); else NXT=0xFFF8; fi
    P=$((F1 + CL * 2))
    write_byte "$P"       $((NXT & 0xFF))
    write_byte "$((P+1))" $(((NXT >> 8) & 0xFF))
done

# ---------- 4. FAT #2 = FAT #1 的完整副本（扇区 129 起） ----------
FAT_TMP="$DIR/.fat1.tmp"
dd if="$DISK_IMG" of="$FAT_TMP" bs="$BPS" skip=1 count="$FAT_SZ" status=none
dd if="$FAT_TMP"  of="$DISK_IMG" bs="$BPS" seek=$((1 + FAT_SZ)) conv=notrunc status=none
rm -f "$FAT_TMP"

# ---------- 5. 根目录（扇区 257 起，512 个目录项） ----------
RO=$((257 * BPS))
write_ascii "$RO"        "SHELL   BIN"
write_byte  "$((RO+11))" 0x20
write_le16  "$((RO+26))" 2          # 起始簇号 = 2
write_le32  "$((RO+28))" "$SHELL_LEN"

# ---------- 6. Shell 数据：簇 2 = 扇区 289 ----------
dd if="$SHELL_BIN" of="$DISK_IMG" bs="$BPS" seek=$((289)) conv=notrunc status=none

echo "disk.img created: $((SIZE / 1024 / 1024)) MB (shell: $SHELL_LEN bytes)"
