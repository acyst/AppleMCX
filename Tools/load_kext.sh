#!/bin/sh
# load_kext.sh — 部署 + 签名 + 加载 AppleMCX.kext
set -e
cd "$(dirname "$0")/.."

if [ "$(id -u)" != "0" ]; then
    echo "==> 需要 sudo 执行 (部署到 /Library/Extensions)"
    exec sudo "$0" "$@"
fi

echo "==> [1/4] 检查产物"
test -f AppleMCX.kext/Contents/MacOS/AppleMCX || {
    echo "错误: 未构建, 先运行: make"
    exit 1
}

echo "==> [2/4] 复制到 /Library/Extensions"
cp -R AppleMCX.kext /Library/Extensions/
touch /Library/Extensions/

echo "==> [3/4] 加载"
kextutil -v /Library/Extensions/AppleMCX.kext 2>&1 || {
    echo "加载失败, 尝试诊断..."
    kextutil -diagnose /Library/Extensions/AppleMCX.kext 2>&1 || true
    exit 1
}

echo "==> [4/4] 验证"
kextstat | grep -i mlx && echo "✅ 驱动已加载" || echo "⚠️ 未在 kextstat 中看到"
ioreg -l | grep -i mlx && echo "✅ 设备树节点存在" || echo "⚠️ 无 Mlx 节点"
