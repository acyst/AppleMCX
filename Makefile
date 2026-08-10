# AppleMCX.kext — generic Mellanox mlx5 family driver
# 平台: macOS (Apple Silicon / Intel)
# 首个实现: ConnectX-5 (mcx5), 框架覆盖 ConnectX-4 ~ ConnectX-8

KEXT_NAME  := AppleMCX
BUNDLE_ID  := com.applemcx.driver
VERSION    := 0.1.0

# 自动检测宿主架构 (Apple Silicon → arm64e, Intel → x86_64)
HOST_ARCH := $(shell uname -m)
ARCH      ?= $(if $(filter arm64%, $(HOST_ARCH)), arm64e, x86_64)
SDK         := $(shell xcrun --sdk macosx --show-sdk-path)
CXX        := $(shell xcrun -sdk macosx -find clang++)
CC          := $(shell xcrun -sdk macosx -find clang)

# ---- 源文件 ----
SRC_CORE   := $(wildcard Sources/core/*.cpp)
SRC_HW     := $(wildcard Sources/hw/*.cpp)
SRC_IB     := $(wildcard Sources/ib/*.cpp)
SRC_NETIF  := $(wildcard Sources/netif/*.cpp)
SRC_UC     := $(wildcard Sources/userclient/*.cpp)
SRC        := $(SRC_CORE) $(SRC_HW) $(SRC_IB) $(SRC_NETIF) $(SRC_UC)
OBJ        := $(patsubst Sources/%,obj/%,$(SRC:.cpp=.o))

# ---- 编译标志 ----
COMMON_INC := -ISources -ISources/hw -ISources/core -ISources/ib \
              -ISources/userclient -ISources/netif

KERNEL_INC := \
    -isysroot $(SDK) \
    -isystem $(SDK)/usr/include \
    -I$(SDK)/System/Library/Frameworks/Kernel.framework/Headers \
    -I$(SDK)/System/Library/Frameworks/Kernel.framework/PrivateHeaders \
    -I$(SDK)/System/Library/Frameworks/IOKit.framework/Headers

CFLAGS := -Wall -Wextra -Wno-unused-parameter -std=c++17 \
    -nostdinc -fno-builtin -fno-exceptions -fno-rtti \
    -mkernel -fapple-kext -fno-stack-protector \
    -fno-threadsafe-statics -fno-c++-static-destructors \
    -DKERNEL -D__KERNEL__ -DAPPLE \
    -arch $(ARCH) -mmacosx-version-min=13.0 \
    $(COMMON_INC) $(KERNEL_INC) \
    -Os

# KEXT 链接: 内核态, 无标准库, 用 cctools ld64 的 -kext 模式
# 优先 KPI stub (libcc_kext/libcplusplus); macOS 26+ SDK 已移除该 stub,
# 改用 -undefined dynamic_lookup, 由内核加载器 kxld 在加载时解析导出符号
KEXT_STUB := $(wildcard $(SDK)/System/Library/Frameworks/Kernel.framework/Libraries/libcc_kext.tbd)
LDFLAGS := -arch $(ARCH) -nostdlib -Wl,-kext \
    $(if $(KEXT_STUB),-lcc_kext -lcplusplus,-undefined dynamic_lookup)

# ---- 目标 ----
.PHONY: all clean load unload sign deploy status tools pkg

all: AppleMCX.kext/Contents/MacOS/$(KEXT_NAME) AppleMCX.kext/Contents/Info.plist

obj/%.o: Sources/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CFLAGS) -c $< -o $@

AppleMCX.kext/Contents/MacOS/$(KEXT_NAME): $(OBJ)
	@mkdir -p AppleMCX.kext/Contents/MacOS
	$(CXX) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@
	@echo "==> Built $@"

AppleMCX.kext/Contents/Info.plist: AppleMCX.kext/Contents/Info.plist.tmpl
	@echo "==> Info.plist already provided"
	@test -f $@ || cp $< $@

# ---- 用户态工具链 ----
tools:
	$(MAKE) -C usermode/toolchain

# ---- 打包 userspace toolchain 为 .pkg ----
pkg:
	./Tools/make_pkg.sh

# ---- 签名 (Apple Silicon 必需) ----
sign:
	@if [ -z "$(CODE_SIGN_ID)" ]; then \
		echo "请设置 CODE_SIGN_ID, 如: make sign CODE_SIGN_ID='Apple Development: x@y'"; \
		exit 1; \
	fi
	codesign --force --sign "$(CODE_SIGN_ID)" \
	    --entitlements Tools/kext.entitlements \
	    --options runtime AppleMCX.kext
	@echo "==> 签名完成"

# ---- 部署与加载 ----
deploy: sign
	@echo "==> 部署到 /Library/Extensions (需 sudo)"
	sudo cp -R AppleMCX.kext /Library/Extensions/
	sudo touch /Library/Extensions/
	@echo "==> 已部署"

load:
	@echo "==> 加载 (Apple Silicon 需先启用开发模式, 见 README)"
	sudo kextload -v /Library/Extensions/AppleMCX.kext || \
	sudo kextutil -v /Library/Extensions/AppleMCX.kext

unload:
	sudo kextunload /Library/Extensions/AppleMCX.kext || true

# ---- 验证 ----
status:
	kextstat | grep -i mlx || echo "驱动未加载"
	ioreg -l | grep -i mlx || echo "设备树中无 Mlx 节点"

clean:
	rm -rf obj build AppleMCX.kext/Contents/MacOS/$(KEXT_NAME)
	@echo "==> 清理完成"
