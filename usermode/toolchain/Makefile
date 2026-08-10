# Toolchain build for the macOS userspace utilities.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

CC      ?= clang
CXX     ?= clang++
# 自动检测宿主架构 (Apple Silicon → arm64, Intel → x86_64)
HOST_ARCH := $(shell uname -m)
ARCH      ?= $(if $(filter arm64, $(HOST_ARCH)), arm64, x86_64)
SDK       := $(shell xcrun --sdk macosx --show-sdk-path)

INC      = -I$(ROOT)/usermode/toolchain/libmft \
           -I$(ROOT)/usermode/toolchain/libverbs \
           -I$(ROOT)/usermode/toolchain/perftest \
           -I$(ROOT)/usermode/libmlx \
           -I$(ROOT)/Sources/userclient \
           -I$(ROOT)/Sources/hw

# 用户态编译: 用系统 SDK 头 (IOKit.framework), 不需内核 SDK
CFLAGS  := -O2 -Wall -Wno-unused-parameter -arch $(strip $(ARCH)) \
           -mmacosx-version-min=13.0 $(INC)

# ---- 库 ----
LIBMLX   = $(ROOT)/usermode/libmlx/libmlx.c
LIBMFT   = $(ROOT)/usermode/toolchain/libmft/libmft.c
LIBVERBS = $(ROOT)/usermode/toolchain/libverbs/verbs_compat.c

# ---- 工具 ----
TOOLS = ibv_devinfo ib_write_bw ib_send_bw ib_read_bw \
        ib_write_lat ib_send_lat ib_read_lat \
        mlxconfig mlxstatus mlxlink mlxreg mlnx_qos

.PHONY: all clean

all: $(TOOLS)

# ibv_devinfo (基于 libmlx)
ibv_devinfo: $(ROOT)/usermode/tests/ibv_devinfo.c $(LIBMLX)
	$(CC) $(CFLAGS) $(ROOT)/usermode/tests/ibv_devinfo.c $(LIBMLX) \
	    -framework IOKit -o $@

# perftest 系 (基于 verbs 兼容层 + libmlx)
ib_write_bw: $(ROOT)/usermode/toolchain/perftest/ib_write_bw.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/perftest/ib_write_bw.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

ib_send_bw: $(ROOT)/usermode/toolchain/perftest/ib_send_bw.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/perftest/ib_send_bw.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

ib_read_bw: $(ROOT)/usermode/toolchain/perftest/ib_read_bw.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/perftest/ib_read_bw.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

# 延迟测试 (基于 verbs 兼容层 + libmlx)
ib_write_lat: $(ROOT)/usermode/toolchain/perftest/ib_write_lat.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/perftest/ib_write_lat.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

ib_send_lat: $(ROOT)/usermode/toolchain/perftest/ib_send_lat.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/perftest/ib_send_lat.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

ib_read_lat: $(ROOT)/usermode/toolchain/perftest/ib_read_lat.c $(LIBVERBS) $(LIBMLX)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/perftest/ib_read_lat.c \
	    $(LIBVERBS) $(LIBMLX) -framework IOKit -o $@

# mlxconfig (基于 libmft)
mlxconfig: $(ROOT)/usermode/toolchain/mlxconfig/mlxconfig.c $(LIBMFT)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/mlxconfig/mlxconfig.c $(LIBMFT) \
	    -framework IOKit -o $@

# mlnx-tools 系 (基于 libmft)
mlxstatus: $(ROOT)/usermode/toolchain/mlnx-tools/mlxstatus.c $(LIBMFT)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/mlnx-tools/mlxstatus.c $(LIBMFT) \
	    -framework IOKit -o $@

mlxlink: $(ROOT)/usermode/toolchain/mlnx-tools/mlxlink.c $(LIBMFT)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/mlnx-tools/mlxlink.c $(LIBMFT) \
	    -framework IOKit -o $@

mlxreg: $(ROOT)/usermode/toolchain/mlnx-tools/mlxreg.c $(LIBMFT)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/mlnx-tools/mlxreg.c $(LIBMFT) \
	    -framework IOKit -o $@

mlnx_qos: $(ROOT)/usermode/toolchain/mlnx-tools/mlnx_qos.c $(LIBMFT)
	$(CC) $(CFLAGS) $(ROOT)/usermode/toolchain/mlnx-tools/mlnx_qos.c $(LIBMFT) \
	    -framework IOKit -o $@

clean:
	rm -f $(TOOLS)
