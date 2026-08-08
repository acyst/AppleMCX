# 工具链构建 — 迁移自 MLNX_OFED 用户态工具 (macOS)
# 位于 usermode/toolchain/

CC      ?= clang
CXX     ?= clang++
# 自动检测宿主架构 (Apple Silicon → arm64, Intel → x86_64)
HOST_ARCH := $(shell uname -m)
ARCH      ?= $(if $(filter arm64, $(HOST_ARCH)), arm64, x86_64)
SDK       := $(shell xcrun --sdk macosx --show-sdk-path)

INC      = -Iusermode/toolchain/libmft \
           -Iusermode/toolchain/libverbs \
           -Iusermode/toolchain/perftest \
           -Iusermode/libmlx \
           -ISources/userclient \
           -ISources/hw

# 用户态编译: 用系统 SDK 头 (IOKit.framework), 不需内核 SDK
CFLAGS  := -O2 -Wall -Wno-unused-parameter -arch $(ARCH) \
           -mmacosx-version-min=13.0 $(INC)

# ---- 库 ----
LIBMLX   = usermode/libmlx/libmlx.c
LIBMFT   = usermode/toolchain/libmft/libmft.c
LIBVERBS = usermode/toolchain/libverbs/verbs_compat.c

# ---- 工具 ----
TOOLS = ibv_devinfo ib_write_bw ib_send_bw ib_read_bw \
        ib_write_lat ib_send_lat ib_read_lat \
        mlxconfig mlxstatus mlxlink mlxreg mlnx_qos

.PHONY: all clean

all: $(TOOLS)

# ibv_devinfo (基于 libmlx)
ibv_devinfo: usermode/tests/ibv_devinfo.c $(LIBMLX)
	$(CC) $(CFLAGS) usermode/tests/ibv_devinfo.c $(LIBMLX) \
	    -framework IOKit -o $@

# perftest 系 (基于 verbs 兼容层 + libmlx)
ib_write_bw: usermode/toolchain/perftest/ib_write_bw.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) usermode/toolchain/perftest/ib_write_bw.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

ib_send_bw: usermode/toolchain/perftest/ib_send_bw.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) usermode/toolchain/perftest/ib_send_bw.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

ib_read_bw: usermode/toolchain/perftest/ib_read_bw.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) usermode/toolchain/perftest/ib_read_bw.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

# 延迟测试 (基于 verbs 兼容层 + libmlx)
ib_write_lat: usermode/toolchain/perftest/ib_write_lat.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) usermode/toolchain/perftest/ib_write_lat.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

ib_send_lat: usermode/toolchain/perftest/ib_send_lat.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) usermode/toolchain/perftest/ib_send_lat.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

ib_read_lat: usermode/toolchain/perftest/ib_read_lat.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) usermode/toolchain/perftest/ib_read_lat.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

# mlxconfig (基于 libmft)
mlxconfig: usermode/toolchain/mlxconfig/mlxconfig.c $(LIBMFT)
	$(CC) $(CFLAGS) usermode/toolchain/mlxconfig/mlxconfig.c $(LIBMFT) \
	    -framework IOKit -o $@

# mlnx-tools 系 (基于 libmft)
mlxstatus: usermode/toolchain/mlnx-tools/mlxstatus.c $(LIBMFT)
	$(CC) $(CFLAGS) usermode/toolchain/mlnx-tools/mlxstatus.c $(LIBMFT) \
	    -framework IOKit -o $@

mlxlink: usermode/toolchain/mlnx-tools/mlxlink.c $(LIBMFT)
	$(CC) $(CFLAGS) usermode/toolchain/mlnx-tools/mlxlink.c $(LIBMFT) \
	    -framework IOKit -o $@

mlxreg: usermode/toolchain/mlnx-tools/mlxreg.c $(LIBMFT)
	$(CC) $(CFLAGS) usermode/toolchain/mlnx-tools/mlxreg.c $(LIBMFT) \
	    -framework IOKit -o $@

mlnx_qos: usermode/toolchain/mlnx-tools/mlnx_qos.c $(LIBMFT)
	$(CC) $(CFLAGS) usermode/toolchain/mlnx-tools/mlnx_qos.c $(LIBMFT) \
	    -framework IOKit -o $@

clean:
	rm -f $(TOOLS)
