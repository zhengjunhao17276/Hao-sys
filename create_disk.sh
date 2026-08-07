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

# 簇分配表（Linux 风格目录布局）：
#   簇 2   = /BIN 目录内容（含 SHELL.BIN 项）
#   簇 3-6 = /ETC /HOME /USR /TMP 空目录内容（各 1 簇）
#   簇 7+  = SHELL.BIN 数据
#   /DEV、/MNT 不占磁盘——它们是系统虚拟目录（devfs/挂载点容器）
DIR_BIN=2
DIR_ETC=3
DIR_HOME=4
DIR_USR=5
DIR_TMP=6
SHELL_START=$((7))

# 标记目录簇为链尾（单簇目录）
for CL in 2 3 4 5 6; do
    P=$((F1 + CL * 2))
    write_byte "$P"       0xF8
    write_byte "$((P+1))" 0xFF
done

# 为 Shell 分配数据簇链：簇 7 起，最后一簇标记 0xFFF8（链尾）
NC=$(( (SHELL_LEN + SPC*BPS - 1) / (SPC*BPS) ))
for ((i = 0; i < NC; i++)); do
    CL=$((SHELL_START + i))
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

# 目录项布局：name(11B) + attr(1B) + 10B 保留/时间 + cluster_lo(2B) + cluster_hi(2B) + size(4B)
# 偏移: 0-10 名字, 11 属性, 26-27 簇低, 20-21 簇高, 28-31 大小
write_dir_entry() {  # $1=偏移  $2=8.3名  $3=属性  $4=簇号
    local o=$1
    write_ascii "$o"        "$2"
    write_byte  "$((o+11))" "$3"
    write_le16  "$((o+20))" 0          # cluster_high
    write_le16  "$((o+26))" "$4"
    write_le32  "$((o+28))" 0          # 目录 size=0
}

# 根目录项：Linux 风格布局
write_dir_entry "$RO"        "BIN         " 0x10 "$DIR_BIN"
write_dir_entry "$((RO+32))" "ETC         " 0x10 "$DIR_ETC"
write_dir_entry "$((RO+64))" "HOME        " 0x10 "$DIR_HOME"
write_dir_entry "$((RO+96))" "USR         " 0x10 "$DIR_USR"
write_dir_entry "$((RO+128))" "TMP         " 0x10 "$DIR_TMP"

# ---------- 6. 子目录内容（. 和 .. 项） ----------
# 每个目录簇内容：".", "..", 其余 0x00
# 目录项内偏移：0-10 名字, 11 属性, 20-21 cluster_high, 26-27 cluster_low
write_dot_entries() {  # $1=目录簇内容起始偏移(字节)  $2=本簇号
    local base=$1
    # "." 项：本目录簇
    write_ascii "$((base))"       ".          "
    write_byte  "$((base+11))"    0x10
    write_le16  "$((base+26))"    "$2"        # . = 本簇
    # ".." 项：根目录（cluster 0），FAT 根目录的 .. 指向 0
    write_ascii "$((base+32))"    "..         "
    write_byte  "$((base+32+11))" 0x10
    # .. cluster 保持 0（镜像已零填充）
}

# 数据区起始扇区 = 289（簇 2）
sec_of_cluster() { echo $((289 + (($1 - 2) * SPC))); }

# 空目录内容（ETC/HOME/USR/TMP/DEV/MNT）：只有 . 和 ..
for CL in $DIR_ETC $DIR_HOME $DIR_USR $DIR_TMP; do
    SEC=$(sec_of_cluster $CL)
    write_dot_entries "$((SEC * BPS))" "$CL"
done

# ---------- 7. /BIN 目录内容（含 SHELL.BIN 项） ----------
BIN_SEC=$(sec_of_cluster $DIR_BIN)
write_dot_entries "$((BIN_SEC * BPS))" "$DIR_BIN"
# SHELL.BIN 目录项（在 /BIN 里，第三项）
BIN_ENTRY=$((BIN_SEC * BPS + 64))
write_dir_entry "$BIN_ENTRY" "SHELL   BIN" 0x20 "$SHELL_START"
write_le32  "$((BIN_ENTRY+28))" "$SHELL_LEN"

# ---------- 8. Shell 数据：簇 7 起 ----------
dd if="$SHELL_BIN" of="$DISK_IMG" bs="$BPS" seek=$(sec_of_cluster $SHELL_START) conv=notrunc status=none

echo "disk.img created: $((SIZE / 1024 / 1024)) MB, Linux layout (/bin/SHELL.BIN, $SHELL_LEN bytes)"
