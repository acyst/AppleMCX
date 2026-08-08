#!/bin/sh
# gen_pci_match.sh — 生成 IOPCIMatch 字符串
#
# 从 Linux mlx5_core 的 PCI ID 表提取 (main.c:2378-2397),
# 输出供 Info.plist 的 IOPCIMatch 使用。
#
# 格式: 0xDDDDVVVV (设备ID 厂商ID)
# 厂商 ID: 0x15B3 (Mellanox/NVIDIA)

VENDOR=0x15B3

# 设备 ID 表 (仅 PF, 不含 VF; VF 驱动通常由 PF 驱动管理)
DEVICES="
1013 ConnectX-4
1015 ConnectX-4LX
1017 ConnectX-5
1019 ConnectX-5Ex
101b ConnectX-6
101d ConnectX-6Dx
101f ConnectX-6LX
1021 ConnectX-7
1023 ConnectX-8
a2d2 BlueField1
a2d6 BlueField2
a2dc BlueField3
a2df BlueField4
"

MATCH=""
echo "==> 生成 IOPCIMatch (PF 设备):"
while read -r did name; do
    [ -z "$did" ] && continue
    entry=$(printf '0x%04x%04x' "0x$did" "$VENDOR")
    MATCH="$MATCH $entry"
    printf "   %-12s %s\n" "$entry" "$name"
done <<EOF
$DEVICES
EOF

echo ""
echo "==> 复制以下内容到 Info.plist 的 IOPCIMatch:"
echo "<string>$MATCH</string>"
