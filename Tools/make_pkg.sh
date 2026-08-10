#!/bin/sh
# make_pkg.sh — 将 userspace toolchain 打包为 .pkg
# 产物: dist/AppleMCX-toolchain-<version>-<arch>.pkg
set -e
cd "$(dirname "$0")/.."

# 与根 Makefile 保持同一版本号
VERSION="$(awk '/^VERSION[ \t]*:=/ {print $3}' Makefile)"
PKG_ID="com.applemcx.toolchain"
ARCH="$(uname -m)"

TOOLS="ibv_devinfo ib_write_bw ib_send_bw ib_read_bw ib_write_lat ib_send_lat ib_read_lat mlxconfig mlxstatus mlxlink mlxreg mlnx_qos"

echo "==> 构建 userspace toolchain"
make -C usermode/toolchain

echo "==> 组装 payload (usr/local/bin)"
PKGROOT="$(mktemp -d)/pkgroot"
BINDIR="$PKGROOT/usr/local/bin"
mkdir -p "$BINDIR"
for t in $TOOLS; do
    [ -x "usermode/toolchain/$t" ] || { echo "==> 缺少 $t, 构建失败"; exit 1; }
    cp "usermode/toolchain/$t" "$BINDIR/"
done

echo "==> 生成 .pkg"
mkdir -p dist
OUT="dist/AppleMCX-toolchain-$VERSION-$ARCH.pkg"
pkgbuild \
    --root "$PKGROOT" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
    --install-location / \
    --ownership recommended \
    "$OUT"
rm -rf "$(dirname "$PKGROOT")"

echo "==> 产物: $OUT"
ls -la "$OUT"
