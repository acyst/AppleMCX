#!/bin/sh
# make_kext_pkg.sh — 签名并打包 AppleMCX.kext 为 .pkg
# 用法:
#   Tools/make_kext_pkg.sh [CODE_SIGN_ID]
#     CODE_SIGN_ID 为空或 "-" 时 ad-hoc 签名 (SIP 关闭/开发环境可用)
# 产物: dist/AppleMCX-kext-<version>-<arch>.pkg
set -e
cd "$(dirname "$0")/.."

VERSION="$(awk '/^VERSION[ \t]*:=/ {print $3}' Makefile)"
ARCH="$(uname -m)"
SIGN_ID="${1:-}"

echo "==> 检查 kext 产物"
test -f AppleMCX.kext/Contents/MacOS/AppleMCX || {
    echo "错误: 未构建, 先运行: make ARCH=arm64e"
    exit 1
}

echo "==> 暂存副本 (剔除 Info.plist.tmpl, 避免污染仓库)"
STAGE="$(mktemp -d)/AppleMCX.kext"
cp -R AppleMCX.kext "$STAGE"
rm -f "$STAGE/Contents/Info.plist.tmpl"

if [ -z "$SIGN_ID" ] || [ "$SIGN_ID" = "-" ]; then
    echo "==> ad-hoc 签名"
    codesign --force --sign - --entitlements Tools/kext.entitlements "$STAGE"
else
    echo "==> 使用证书签名: $SIGN_ID"
    codesign --force --sign "$SIGN_ID" --entitlements Tools/kext.entitlements "$STAGE"
fi
codesign -dv "$STAGE" >/dev/null 2>&1 || { echo "签名验证失败"; exit 1; }

echo "==> 生成 kext .pkg"
mkdir -p dist
OUT="dist/AppleMCX-kext-$VERSION-$ARCH.pkg"
pkgbuild --component "$STAGE" \
    --identifier com.applemcx.driver \
    --version "$VERSION" \
    --install-location /Library/Extensions \
    "$OUT"
rm -rf "$(dirname "$STAGE")"

echo "==> 产物: $OUT"
ls -la "$OUT"
