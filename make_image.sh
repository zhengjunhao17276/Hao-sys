#!/usr/bin/env bash
# ============================================================
# make_image.sh - 打包 HaoOS 可启动镜像（HaoOS.img）
#
# 布局：
#   扇区 0      = stage1（MBR 引导器 446B + 分区表）
#   扇区 1-2    = stage2（二级引导器：读内核 → E820 → ELF 加载 → 跳转）
#   扇区 3..    = kernel.elf 原始字节（stage2 读取）
#   扇区 2048.. = FAT16 分区（类型 0xEF，HaoOS 根文件系统）
#
# 启动链：BIOS → stage1 → stage2 → Multiboot1 内核 → 挂载分区 → Shell
# 启动：qemu-system-i386 -hda HaoOS.img（或写入 U 盘真机启动）
# ============================================================
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL="$DIR/kernel.elf"
SHELL_BIN="$DIR/user/shell.bin"
STAGE1="$DIR/boot/stage1.bin"
STAGE2="$DIR/boot/stage2.bin"
OUT="$DIR/HaoOS.img"

for f in "$KERNEL" "$SHELL_BIN" "$STAGE1" "$STAGE2"; do
    [ -f "$f" ] || { echo "错误: 缺少 $f（先 make，再 make_boot.sh 生成 stage）" >&2; exit 1; }
done

# ---------- 布局参数 ----------
BPS=512; SPC=2; RES=1; NF=2; REC=512; FAT_SZ=128
SIZE=$((32 * 1024 * 1024))     # 整盘 32MB
PART_LBA=2048                  # 分区起始扇区（1MB 对齐）
PART=$((SIZE / BPS - PART_LBA))
KERNEL_BLOB_LBA=3              # 内核 blob 起始扇区

OFF=$((PART_LBA * BPS))
ROOT_SEC=$((RES + NF * FAT_SZ))
DATA_SEC=$((ROOT_SEC + ((REC * 32 + BPS - 1) / BPS)))
sec_of_cluster() { echo $((DATA_SEC + (($1 - 2) * SPC))); }

# ---------- 工具函数 ----------
write_byte() {
    printf "\\$(printf '%03o' "$2")" | dd of="$OUT" bs=1 seek="$1" conv=notrunc status=none
}
write_le16() {
    local o=$1 v=$2
    write_byte "$o"       $((v & 0xFF))
    write_byte "$((o+1))" $(((v >> 8) & 0xFF))
}
write_le32() {
    local o=$1 v=$2
    write_byte "$o"       $((v & 0xFF))
    write_byte "$((o+1))" $(((v >> 8) & 0xFF))
    write_byte "$((o+2))" $(((v >> 16) & 0xFF))
    write_byte "$((o+3))" $(((v >> 24) & 0xFF))
}
write_ascii() { printf '%s' "$2" | dd of="$OUT" bs=1 seek="$1" conv=notrunc status=none; }
write_fat16() {
    local P=$((OFF + 1 * BPS + $1 * 2))
    write_byte "$P"       $((($2 & 0xFF)))
    write_byte "$((P+1))" $((($2 >> 8) & 0xFF))
    local Q=$((OFF + (1 + FAT_SZ) * BPS + $1 * 2))
    write_byte "$Q"       $((($2 & 0xFF)))
    write_byte "$((Q+1))" $((($2 >> 8) & 0xFF))
}
write_dir_entry() {
    local o=$1
    write_ascii "$o"        "$2"
    write_byte  "$((o+11))" "$3"
    write_le16  "$((o+20))" 0
    write_le16  "$((o+26))" "$4"
    write_le32  "$((o+28))" "$5"
}
write_dot_entries() {
    local base=$1
    write_ascii "$((base))"       ".          "
    write_byte  "$((base+11))"    0x10
    write_le16  "$((base+26))"    "$2"
    write_ascii "$((base+32))"    "..         "
    write_byte  "$((base+32+11))" 0x10
}
alloc_chain() {
    local cl=$1 n=$2
    for ((i = 0; i < n; i++)); do
        local c=$((cl + i))
        if [ $i -lt $((n - 1)) ]; then write_fat16 "$c" $((c + 1)); else write_fat16 "$c" 0xFFF8; fi
    done
}

# ---------- 1. 空镜像 ----------
rm -f "$OUT"
truncate -s "$SIZE" "$OUT"

# ---------- 2. MBR（stage1）+ stage2 + 内核 blob + 分区表 ----------
dd if="$STAGE1" of="$OUT" bs=1 seek=0 conv=notrunc status=none
dd if="$STAGE2" of="$OUT" bs=512 seek=1 conv=notrunc status=none

KERNEL_LEN=$(stat -c %s "$KERNEL")
KERNEL_SECTORS=$(( (KERNEL_LEN + 511) / 512 ))
dd if="$KERNEL" of="$OUT" bs=512 seek=$KERNEL_BLOB_LBA conv=notrunc status=none

write_byte  446 0x80
write_byte  450 0xEF
write_le32  454 "$PART_LBA"
write_le32  458 "$PART"
write_byte  510 0x55
write_byte  511 0xAA

# ---------- 3. FAT BPB ----------
write_byte  $((OFF+0))  0xEB
write_byte  $((OFF+1))  0x3C
write_byte  $((OFF+2))  0x90
write_ascii $((OFF+3))  "HaoOS   "
write_le16  $((OFF+11)) "$BPS"
write_byte  $((OFF+13)) "$SPC"
write_le16  $((OFF+14)) "$RES"
write_byte  $((OFF+16)) "$NF"
write_le16  $((OFF+17)) "$REC"
write_le16  $((OFF+19)) "$((PART & 0xFFFF))"
write_byte  $((OFF+21)) 0xF8
write_le16  $((OFF+22)) "$FAT_SZ"
write_byte  $((OFF+24)) 63
write_byte  $((OFF+25)) 0
write_byte  $((OFF+26)) 16
write_byte  $((OFF+27)) 0
write_le32  $((OFF+32)) "$PART"
write_byte  $((OFF+36)) 0x80
write_byte  $((OFF+38)) 0x29
write_byte  $((OFF+39)) 0xDE
write_byte  $((OFF+40)) 0xAD
write_byte  $((OFF+41)) 0xBE
write_byte  $((OFF+42)) 0xEF
write_ascii $((OFF+43)) "HAOOS      "
write_ascii $((OFF+54)) "FAT16   "
write_byte  $((OFF+510)) 0x55
write_byte  $((OFF+511)) 0xAA

# ---------- 4. FAT 表 ----------
write_byte $((OFF + 1*BPS))       0xF8
write_byte $((OFF + 1*BPS + 1))   0xFF
write_byte $((OFF + 1*BPS + 2))   0xFF
write_byte $((OFF + 1*BPS + 3))   0xFF

# ---------- 5. 簇分配 ----------
DIR_BIN=2; DIR_ETC=3; DIR_HOME=4; DIR_USR=5; DIR_TMP=6

SHELL_LEN=$(stat -c %s "$SHELL_BIN")
cluster_count() { echo $(( (($1) + SPC*BPS - 1) / (SPC*BPS) )); }
NC_SHELL=$(cluster_count "$SHELL_LEN")
NC_KERNEL=$(cluster_count "$KERNEL_LEN")

SHELL_START=9
KERNEL_START=$((SHELL_START + NC_SHELL))

for CL in 2 3 4 5 6; do write_fat16 "$CL" 0xFFF8; done
alloc_chain "$SHELL_START"   "$NC_SHELL"
alloc_chain "$KERNEL_START"  "$NC_KERNEL"

# ---------- 6. 根目录 ----------
RO=$((OFF + ROOT_SEC * BPS))
write_dir_entry "$RO"        "BIN         " 0x10 "$DIR_BIN" 0
write_dir_entry "$((RO+32))" "ETC         " 0x10 "$DIR_ETC" 0
write_dir_entry "$((RO+64))" "HOME        " 0x10 "$DIR_HOME" 0
write_dir_entry "$((RO+96))" "USR         " 0x10 "$DIR_USR" 0
write_dir_entry "$((RO+128))" "TMP        " 0x10 "$DIR_TMP" 0

# ---------- 7. 子目录（. 和 ..）+ /BIN/SHELL.BIN ----------
for CL in $DIR_ETC $DIR_HOME $DIR_USR $DIR_TMP $DIR_BIN; do
    SEC=$(sec_of_cluster "$CL")
    write_dot_entries "$((OFF + SEC * BPS))" "$CL"
done
BIN_SEC=$(sec_of_cluster "$DIR_BIN")
write_dir_entry "$((OFF + BIN_SEC * BPS + 64))" "SHELL   BIN" 0x20 "$SHELL_START" "$SHELL_LEN"

# ---------- 8. 数据 ----------
dd if="$SHELL_BIN" of="$OUT" bs="$BPS" seek=$((PART_LBA + $(sec_of_cluster "$SHELL_START"))) conv=notrunc status=none
dd if="$KERNEL"    of="$OUT" bs="$BPS" seek=$((PART_LBA + $(sec_of_cluster "$KERNEL_START"))) conv=notrunc status=none

echo "HaoOS.img created: $((SIZE / 1024 / 1024)) MB"
echo "  boot: stage1(MBR)+stage2+kernel.elf blob ($KERNEL_SECTORS sectors @ LBA $KERNEL_BLOB_LBA)"
echo "  fs:   FAT16 @ LBA $PART_LBA (/BIN/SHELL.BIN + /KERNEL.ELF)"
echo "  启动: qemu-system-i386 -hda HaoOS.img"
