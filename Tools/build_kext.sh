#!/bin/sh
# build_kext.sh — 一键构建 AppleMCX.kext
set -e
cd "$(dirname "$0")/.."

ARCH="${1:-arm64e}"
echo "==> 构建 AppleMCX.kext (arch=$ARCH)"
make clean >/dev/null 2>&1 || true
make ARCH="$ARCH"
echo "==> 构建产物:"
ls -la AppleMCX.kext/Contents/MacOS/
echo "==> 完成。加载前需签名: make sign CODE_SIGN_ID='<你的证书>'"
