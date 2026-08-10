/*
 * MlxCmd.cpp — Firmware command interface implementation (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/net/ethernet/mellanox/mlx5/core/cmd.c
 * Key points:
 *   - Command descriptor layout: device.h:525 mlx5_cmd_layout (64 bytes)
 *   - Large commands (>16B) use the mailbox chain: mlx5_cmd_prot_block (device.h:781)
 *   - Descriptor signature = XOR checksum (cmd.c:228 set_signature)
 *   - Ring the doorbell after submission (iseg->cmd_dbell)
 */
#include "MlxCmd.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxKernelCompat.hpp"

#include <string.h>
#include <libkern/OSTypes.h>
#include <libkern/OSByteOrder.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOKitDebug.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxCmd, OSObject)

/* ---- Private helpers ---- */

void MlxCmd::free()
{
    fUp = false;
    for (uint32_t i = 0; i < MLX_MAX_COMMANDS; i++) {
        MlxCmdEnt *ent = fEntArr[i];
        if (!ent)
            continue;
        freeMailbox(ent);
        IOFree(ent, sizeof(MlxCmdEnt));
        fEntArr[i] = NULL;
    }
    mlxUnmapDMA(fCmdBufMap);
    fCmdBufMap = NULL;
    if (fCmdBufDesc) {
        fCmdBufDesc->release();
        fCmdBufDesc = NULL;
    }
    if (fAllocLock) {
        IOSimpleLockFree(fAllocLock);
        fAllocLock = NULL;
    }
    if (fTokenLock) {
        IOSimpleLockFree(fTokenLock);
        fTokenLock = NULL;
    }
    super::free();
}

static inline uint8_t
xor8_buf(const void *buf, size_t off, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf + off;
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++)
        acc ^= p[i];
    return acc;
}

bool MlxCmd::init(MlxPCIDriver *owner, IOMemoryMap *bar0, uint32_t cmdqSize)
{
    if (!super::init())
        return false;

    fOwner = owner;
    fBar0  = bar0;
    fUp    = false;
    fQuarantined = false;
    fModeEvents = false;
    fToken = 0;
    fBitmask = 0;
    fCmdBufDesc = NULL;
    fCmdBufMap = NULL;
    fAllocLock = NULL;
    fTokenLock = NULL;
    fCmdBuf = NULL;
    memset(fEntArr, 0, sizeof(fEntArr));

    /* 1. Validate command interface revision: high 16 bits of cmdif_rev_fw_sub
     *    == CMD_IF_REV(5), See cmd.c:2239-2245 */
    uint32_t cmdifRevFw = mlxMMIORead32BE(
        bar0, offsetof(struct MlxInitSeg, cmdif_rev_fw_sub));
    fCmdifRev = (uint16_t)(cmdifRevFw >> 16);
    if (fCmdifRev != MLX_CMD_IF_REV) {
        IOLog("MlxCmd: command interface revision mismatch (firmware=%u, need=%u)\n",
              fCmdifRev, MLX_CMD_IF_REV);
        return false;
    }

    /* 2. Read the command queue parameters declared by firmware (low 12 bits of
     *    iseg->cmdq_addr_l_sz), See cmd.c:2255-2269 */
    uint32_t cmdqAddrLSz = mlxMMIORead32BE(
        bar0, offsetof(struct MlxInitSeg, cmdq_addr_l_sz));
    uint8_t cmdqParams = static_cast<uint8_t>(cmdqAddrLSz & 0xFF);
    fLogSz = (cmdqParams >> 4) & 0xF;
    fLogStride = cmdqParams & 0xF;
    if (fLogSz >= 31 || fLogStride < 6 || fLogSz + fLogStride > 12) {
        IOLog("MlxCmd: invalid command queue geometry log_sz=%u stride=%u\n",
              fLogSz, fLogStride);
        return false;
    }
    uint32_t queueSize = 1u << fLogSz;
    if (queueSize == 0 || queueSize > MLX_MAX_COMMANDS ||
        fLogSz > 5 ||
        cmdqSize < (1u << (fLogSz + fLogStride))) {
        IOLog("MlxCmd: command queue too large log_sz=%u\n", fLogSz);
        return false;
    }

    /* 3. Allocate a DMA-coherent command queue page
     *    See cmd.c:2188 alloc_cmd_page */
    fCmdBufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, cmdqSize, 0xFFFFFFF000ULL);
    if (!fCmdBufDesc) {
        IOLog("MlxCmd: command queue allocation failed\n");
        return false;
    }
    fCmdBuf = fCmdBufDesc->getBytesNoCopy();
    if (mlxMapDMAContiguous(fCmdBufDesc, &fCmdBufMap, &fCmdDMA) !=
        kIOReturnSuccess) {
        fCmdBufDesc->release();
        fCmdBufDesc = NULL;
        return false;
    }
    memset(fCmdBuf, 0, cmdqSize);

    /* 4. Write the command queue DMA address to firmware
     *    See cmd.c:2300-2304 */
    mlxMMIOWrite32BE(bar0, offsetof(struct MlxInitSeg, cmdq_addr_h),
                     (uint32_t)(fCmdDMA >> 32));
    mlxMMIOWrite32BE(bar0, offsetof(struct MlxInitSeg, cmdq_addr_l_sz),
                     (uint32_t)(fCmdDMA & 0xFFFFFFFF));

    /* 5. Initialize the command slot bitmap and locks */
    fMaxRegCmds = queueSize - 1; /* reserve the last slot for page commands */
    fBitmask = fMaxRegCmds ? ((1u << fMaxRegCmds) - 1) : 0;
    fAllocLock = IOSimpleLockAlloc();
    fTokenLock = IOSimpleLockAlloc();
    if (!fAllocLock || !fTokenLock) {
        mlxUnmapDMA(fCmdBufMap);
        fCmdBufMap = NULL;
        fCmdBufDesc->release();
        fCmdBufDesc = NULL;
        return false;
    }
    memset(fEntArr, 0, sizeof(fEntArr));

    fUp = true;
    IOLog("MlxCmd: command interface ready (rev=%u, log_sz=%u, stride=%u, queue=%u)\n",
          fCmdifRev, fLogSz, fLogStride, queueSize);
    return true;
}

bool MlxCmd::allocIdx(uint32_t *idx)
{
    bool found = false;
    IOSimpleLockLock(fAllocLock);
    if (fBitmask) {
        uint32_t bit = __builtin_ctz(fBitmask);
        fBitmask &= ~(1u << bit);
        *idx = bit;
        found = true;
    }
    IOSimpleLockUnlock(fAllocLock);
    return found;
}

void MlxCmd::freeIdx(uint32_t idx)
{
    IOSimpleLockLock(fAllocLock);
    fBitmask |= (1u << idx);
    IOSimpleLockUnlock(fAllocLock);
}

void MlxCmd::setSignature(MlxCmdLayout *lay)
{
    /* See cmd.c:228 set_signature:
     *   sig = ~xor8_buf(lay, 0, sizeof(*lay)) */
    lay->sig = 0;
    lay->sig = (uint8_t)~xor8_buf(lay, 0, sizeof(*lay));
}

void MlxCmd::setMailboxSignature(MlxCmdMailbox *mb, size_t len)
{
    /* See cmd.c:207 calc_block_sig */
    size_t ctrl_xor_len = sizeof(MlxCmdMailbox) - sizeof(mb->data) - 2;
    size_t rsvd0_off = offsetof(MlxCmdMailbox, rsvd0);
    mb->ctrl_sig = (uint8_t)~xor8_buf(mb, rsvd0_off, ctrl_xor_len);
    mb->sig = (uint8_t)~xor8_buf(mb, 0, len - 1);
}

kern_return_t MlxCmd::allocMailbox(MlxCmdEnt *ent, uint32_t inSize, uint32_t outSize)
{
    ent->inNumBlocks = (inSize > 16) ?
        (inSize - 16 + MLX_CMD_DATA_BLOCK_SIZE - 1) / MLX_CMD_DATA_BLOCK_SIZE : 0;
    ent->outNumBlocks = (outSize > 16) ?
        (outSize - 16 + MLX_CMD_DATA_BLOCK_SIZE - 1) / MLX_CMD_DATA_BLOCK_SIZE : 0;
    if (ent->inNumBlocks > MLX_CMD_MAX_BLOCKS ||
        ent->outNumBlocks > MLX_CMD_MAX_BLOCKS)
        return kIOReturnNoSpace;

    for (uint32_t direction = 0; direction < 2; direction++) {
        uint32_t count = direction ? ent->outNumBlocks : ent->inNumBlocks;
        for (uint32_t i = 0; i < count; i++) {
            IOBufferMemoryDescriptor **descs = direction ?
                ent->outMailboxDesc : ent->inMailboxDesc;
            IODMACommand **maps = direction ? ent->outMailboxMap : ent->inMailboxMap;
            MlxCmdMailbox **boxes = direction ? ent->outMailbox : ent->inMailbox;
            uint64_t *addresses = direction ? ent->outMailboxDMA : ent->inMailboxDMA;

            descs[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task, kIODirectionInOut, sizeof(MlxCmdMailbox),
                0xFFFFFFF000ULL);
            if (!descs[i]) {
                freeMailbox(ent);
                return kIOReturnNoMemory;
            }
            boxes[i] = reinterpret_cast<MlxCmdMailbox *>(
                descs[i]->getBytesNoCopy());
            memset(boxes[i], 0, sizeof(MlxCmdMailbox));
            IOReturn kr = mlxMapDMAContiguous(descs[i], &maps[i], &addresses[i]);
            if (kr != kIOReturnSuccess) {
                freeMailbox(ent);
                return kr;
            }
        }
    }
    return kIOReturnSuccess;
}

void MlxCmd::freeMailbox(MlxCmdEnt *ent)
{
    for (uint32_t direction = 0; direction < 2; direction++) {
        IOBufferMemoryDescriptor **descs = direction ?
            ent->outMailboxDesc : ent->inMailboxDesc;
        IODMACommand **maps = direction ? ent->outMailboxMap : ent->inMailboxMap;
        MlxCmdMailbox **boxes = direction ? ent->outMailbox : ent->inMailbox;
        uint32_t count = direction ? ent->outNumBlocks : ent->inNumBlocks;
        for (uint32_t i = 0; i < count; i++) {
            mlxUnmapDMA(maps[i]);
            maps[i] = NULL;
            if (descs[i]) {
                descs[i]->release();
                descs[i] = NULL;
            }
            boxes[i] = NULL;
        }
    }
    ent->inNumBlocks = 0;
    ent->outNumBlocks = 0;
}

kern_return_t MlxCmd::submit(uint32_t idx, MlxCmdInOut *cmd)
{
    /* See cmd_work_handler (cmd.c:969-1056)
     * Command descriptor: MlxCmdLayout (device.h:525) */
    MlxCmdLayout *lay = (MlxCmdLayout *)((uint8_t *)fCmdBuf + (idx << fLogStride));
    MlxCmdEnt *ent = fEntArr[idx];
    memset(lay, 0, sizeof(*lay));

    /* Command header: first 16 bytes (opcode/op_mod) into in[4] */
    memcpy(lay->in, cmd->in, (cmd->inSize < 16) ? cmd->inSize : 16);

    /* Large input → mailbox */
    for (uint32_t i = 0; ent && i < ent->inNumBlocks; i++) {
        MlxCmdMailbox *mb = ent->inMailbox[i];
        uint32_t copied = 16 + i * MLX_CMD_DATA_BLOCK_SIZE;
        uint32_t dataLen = cmd->inSize - copied;
        if (dataLen > MLX_CMD_DATA_BLOCK_SIZE)
            dataLen = MLX_CMD_DATA_BLOCK_SIZE;
        memcpy(mb->data, static_cast<const uint8_t *>(cmd->in) + copied,
               dataLen);
        mb->next = OSSwapHostToBigInt64(
            (i + 1 < ent->inNumBlocks) ? ent->inMailboxDMA[i + 1] : 0);
        mb->block_num = OSSwapHostToBigInt32(i);
        mb->token = ent->token;
        setMailboxSignature(mb, sizeof(*mb));
    }
    lay->in_ptr = OSSwapHostToBigInt64(
        (ent && ent->inNumBlocks) ? ent->inMailboxDMA[0] : 0);
    lay->inlen = OSSwapHostToBigInt32(cmd->inSize);

    /* Large output → mailbox */
    for (uint32_t i = 0; ent && i < ent->outNumBlocks; i++) {
        MlxCmdMailbox *mb = ent->outMailbox[i];
        mb->next = OSSwapHostToBigInt64(
            (i + 1 < ent->outNumBlocks) ? ent->outMailboxDMA[i + 1] : 0);
        mb->block_num = OSSwapHostToBigInt32(i);
        mb->token = ent->token;
        setMailboxSignature(mb, sizeof(*mb));
    }
    lay->out_ptr = OSSwapHostToBigInt64(
        (ent && ent->outNumBlocks) ? ent->outMailboxDMA[0] : 0);
    lay->outlen = OSSwapHostToBigInt32(cmd->outSize);

    /* type + token */
    lay->type = 0x7;                 /* MLX5_PCI_CMD_XPORT */
    lay->token = ent ? ent->token : 0;

    /* Hand ownership to firmware + signature */
    lay->status_own = MLX_CMD_OWNER_HW;
    setSignature(lay);

    /* Record the in-flight entity */
    if (ent) {
        ent->done = false;
        ent->status = kIOReturnSuccess;
    }

    /* Doorbell: set the command slot bit → firmware fetches the command via DMA
     * See cmd.c:1044-1047 */
    mlxMemoryBarrier();
    mlxMMIOWrite32BE(fBar0, offsetof(struct MlxInitSeg, cmd_dbell), 1u << idx);

    return kIOReturnSuccess;
}

kern_return_t MlxCmd::exec(MlxCmdInOut *cmd, uint32_t timeoutMs)
{
    if (!fUp)
        return kIOReturnNotReady;
    if (fQuarantined && cmd &&
        cmd->opcode != MLX_CMD_OP_TEARDOWN_HCA &&
        cmd->opcode != MLX_CMD_OP_DISABLE_HCA)
        return kIOReturnNotReady;
    if (!cmd || !cmd->in || cmd->inSize == 0 ||
        cmd->inSize > MLX_CMD_MAX_SIZE || cmd->outSize > MLX_CMD_MAX_SIZE ||
        (cmd->outSize && !cmd->out))
        return kIOReturnBadArgument;

    uint32_t idx;
    if (!allocIdx(&idx))
        return kIOReturnNoSpace;

    /* Build the in-flight entity */
    MlxCmdEnt *ent = static_cast<MlxCmdEnt *>(IOMallocZero(sizeof(MlxCmdEnt)));
    if (!ent) {
        freeIdx(idx);
        return kIOReturnNoMemory;
    }
    ent->idx = idx;
    IOSimpleLockLock(fTokenLock);
    ent->token = fToken++;
    IOSimpleLockUnlock(fTokenLock);
    ent->lay = (MlxCmdLayout *)((uint8_t *)fCmdBuf + (idx << fLogStride));
    ent->pending = true;
    fEntArr[idx] = ent;

    /* Allocate mailbox (large command) */
    kern_return_t kr = allocMailbox(ent, cmd->inSize, cmd->outSize);
    if (kr != kIOReturnSuccess) {
        freeIdx(idx);
        fEntArr[idx] = NULL;
        IOFree(ent, sizeof(MlxCmdEnt));
        return kr;
    }

    kr = submit(idx, cmd);
    if (kr != kIOReturnSuccess) {
        freeMailbox(ent);
        freeIdx(idx);
        fEntArr[idx] = NULL;
        IOFree(ent, sizeof(MlxCmdEnt));
        return kr;
    }

    /* Wait for completion (poll the ownership bit, See poll_timeout cmd.c:237)
     * MVP uses polling; EQ interrupt event mode will be added later */
    uint32_t waitedMs = 0;
    while (!ent->done) {
        mlxMemoryBarrier();
        uint8_t own = ent->lay->status_own;
        if (!(own & MLX_CMD_OWNER_HW)) {
            ent->done = true;
        }
        if (ent->done)
            break;
        if (timeoutMs && waitedMs++ >= timeoutMs)
            break;
        IOSleep(1);
    }

    if (!ent->done) {
        /* Firmware still owns the slot and DMA buffers. Quarantine them until
         * reset/teardown rather than allowing a late DMA into freed memory.
         * The command interface is no longer safe for new submissions. */
        ent->timedOut = true;
        fQuarantined = true;
        IOLog("MlxCmd: opcode 0x%x timed out; command interface quarantined\n",
              cmd->opcode);
        return kIOReturnTimeout;
    }

    /* Read back the response: first 16 bytes out[4] + large output from out mailbox */
    if (cmd->out && cmd->outSize) {
        uint32_t copyLen = (cmd->outSize < 16) ? cmd->outSize : 16;
        memcpy(cmd->out, ent->lay->out, copyLen);
        for (uint32_t i = 0; i < ent->outNumBlocks; i++) {
            uint32_t copied = 16 + i * MLX_CMD_DATA_BLOCK_SIZE;
            uint32_t mbLen = cmd->outSize - copied;
            if (mbLen > MLX_CMD_DATA_BLOCK_SIZE)
                mbLen = MLX_CMD_DATA_BLOCK_SIZE;
            memcpy(static_cast<uint8_t *>(cmd->out) + copied,
                   ent->outMailbox[i]->data, mbLen);
        }
    }

    /* Status: status_own >> 1 */
    uint8_t status = (ent->lay->status_own >> 1) & 0x7F;
    kern_return_t result = (status == 0) ? kIOReturnSuccess : kIOReturnIOError;
    if (result != kIOReturnSuccess) {
        IOLog("MlxCmd: opcode=%04x firmware error status=%u\n", cmd->opcode, status);
    }

    freeMailbox(ent);
    freeIdx(idx);
    fEntArr[idx] = NULL;
    IOFree(ent, sizeof(MlxCmdEnt));
    return result;
}

void MlxCmd::handleCompletion(uint32_t vector)
{
    /* Event mode: firmware sends a CMD EQE, vector = completion bitmap
     * See cmd.c:1660 mlx5_cmd_comp_handler */
    uint32_t idx = __builtin_ctz(vector);
    if (idx >= MLX_MAX_COMMANDS)
        return;
    MlxCmdEnt *ent = fEntArr[idx];
    if (ent) {
        ent->done = true;
        ent->status = kIOReturnSuccess;
    }
}
