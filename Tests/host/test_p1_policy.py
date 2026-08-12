#!/usr/bin/env python3
import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


cmd = read("Sources/core/MlxCmd.cpp")
assert "mlxP1ParseOutbox" in cmd
assert "cmd->opcode == MLX_CMD_OP_MANAGE_PAGES" in cmd
assert "idx = fMaxRegCmds" in cmd
assert "while (vector)" in cmd

core = read("Sources/core/MlxPCIDriver.cpp")
assert "satisfyStartupPages(kMlxFwPageBoot)" in core
assert "satisfyStartupPages(kMlxFwPageInit)" in core
assert re.search(r"uint8_t in\[MLX_P1_INIT_HCA_IN_BYTES\]", core)
assert "MLX_P1_QUERY_HCA_CAP_OUT_BYTES" in core
assert "disableBusMasterAndVerify" in core
assert "configRead16(kIOPCIConfigCommand)" in core
assert "fHCA->caps().nicFlowTable" in core
assert "MLX_P1_CAP_ETHERNET_OFFLOADS" in core
assert "memcpy(in + MLX_P1_CMD_HEADER_BYTES, curCap, 256)" in core
assert "configureInterrupts(kIOInterruptTypePCIMessagedX, 2, 2)" in core
assert "gracefulBoundary" in core
assert "retaining mappings" in core

eq = read("Sources/core/MlxEQ.cpp")
assert "OSWriteBigInt64(in, maskOffset + (i * 8), mask[i])" in eq
assert "updateCi(&fAsyncEqs[i], true)" in eq
assert "updateCi(&fCompEqs[i], true)" in eq
assert "uint64_t asyncMask[4] = {}" in eq
assert "0xFFFFFFFF" not in eq
assert "synchronizeCallbacks" in eq
assert "if (kr != kIOReturnTimeout)" in eq
assert "getCmd()->handleCompletion" not in eq
assert "type == MLX_EVENT_TYPE_DEVICE_FATAL" in eq

pages = read("Sources/core/MlxFwPages.cpp")
assert "kMlxFwPageAmbiguous" in pages
assert "thread_call_enter" in pages
assert "registerNotifier(MLX_EVENT_TYPE_PAGE_REQUEST" in pages
assert "reclaimAll" in pages
assert "fTrackedPages > MLX_FW_MAX_TRACKED_PAGES - pageCount" in pages
assert "fWorkerReschedule" in pages
assert "THREAD_CALL_OPTIONS_ONCE" in pages
assert "THREAD_CALL_PRIORITY_KERNEL" in pages

regs = read("Sources/hw/MlxRegs.hpp")
assert "MLX_EVENT_TYPE_WQ_CATAS_ERROR = 0x05" in regs
assert "MLX_EVENT_TYPE_NIC_VPORT_CHANGE = 0x0d" in regs
assert "MLX_EVENT_TYPE_DEVICE_FATAL      = 0x08" in regs

uar = read("Sources/core/MlxUAR.cpp")
assert "PAGE_SIZE" in uar
assert "getUarVirtualAddress()" in uar
