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

#include <string.h>
#include <libkern/OSTypes.h>
#include <libkern/OSByteOrder.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOKitDebug.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxCmd, OSObject)

/* ---- Private helpers ---- */

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
    fModeEvents = false;
    fToken = 0;
    fBitmask = 0;

    /* 1. Validate command interface revision: high 16 bits of cmdif_rev_fw_sub
     *    == CMD_IF_REV(5), See cmd.c:2239-2245 */
    uint32_t cmdifRevFw = IORead32(bar0,
                                   offsetof(struct MlxInitSeg, cmdif_rev_fw_sub));
    fCmdifRev = (uint16_t)(cmdifRevFw >> 16);
    if (fCmdifRev != MLX_CMD_IF_REV) {
        IOLog("MlxCmd: command interface revision mismatch (firmware=%u, need=%u)\n",
              fCmdifRev, MLX_CMD_IF_REV);
        return false;
    }

    /* 2. Read the command queue parameters declared by firmware (low 12 bits of
     *    iseg->cmdq_addr_l_sz), See cmd.c:2255-2269 */
    uint32_t cmdqAddrLSz = IORead32(bar0,
                                    offsetof(struct MlxInitSeg, cmdq_addr_l_sz));
    fLogSz     = (uint8_t)(cmdqAddrLSz & 0x3F);
    fLogStride = (uint8_t)((cmdqAddrLSz >> 6) & 0x3F);
    uint32_t queueSize = 1u << fLogSz;
    if (queueSize > MLX_MAX_COMMANDS) {
        IOLog("MlxCmd: command queue too large log_sz=%u\n", fLogSz);
        return false;
    }

    /* 3. Allocate a DMA-coherent command queue page
     *    See cmd.c:2188 alloc_cmd_page */
    fCmdBufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, cmdqSize, 0xFFFFFFF000ULL, 0);
    if (!fCmdBufDesc) {
        IOLog("MlxCmd: command queue allocation failed\n");
        return false;
    }
    if (fCmdBufDesc->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        fCmdBufDesc->release();
        fCmdBufDesc = NULL;
        return false;
    }
    fCmdBuf = fCmdBufDesc->getBytesNoCopy();
    fCmdDMA = fCmdBufDesc->getPhysicalSegment(0, 0);
    memset(fCmdBuf, 0, cmdqSize);

    /* 4. Write the command queue DMA address to firmware
     *    See cmd.c:2300-2304 */
    IOWrite32(bar0, offsetof(struct MlxInitSeg, cmdq_addr_h),
              (uint32_t)(fCmdDMA >> 32));
    IOWrite32(bar0, offsetof(struct MlxInitSeg, cmdq_addr_l_sz),
              (uint32_t)(fCmdDMA & 0xFFFFFFFF));
    OSMemoryBarrier();

    /* 5. Initialize the command slot bitmap and locks */
    fMaxRegCmds = queueSize - 1;
    fBitmask = (1u << fMaxRegCmds) - 1;
    fAllocLock = IOSimpleLockAlloc();
    fTokenLock = IOSimpleLockAlloc();
    if (!fAllocLock || !fTokenLock) {
        fCmdBufDesc->complete();
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
    lay->sig = (uint8_t)~xor8_buf(lay, 0, MLX_COMMAND_DESCRIPTOR_SIZE - 1);
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
    /* in mailbox (when needed) */
    if (inSize > 16) {
        ent->inMailboxDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, MLX_CMD_DATA_BLOCK_SIZE,
            0xFFFFFFF000ULL, 0);
        if (!ent->inMailboxDesc)
            return kIOReturnNoMemory;
        if (ent->inMailboxDesc->prepare(kIODirectionInOut) != kIOReturnSuccess)
            return kIOReturnNoMemory;
        ent->inMailbox = ent->inMailboxDesc->getBytesNoCopy();
        ent->inMailboxDMA = ent->inMailboxDesc->getPhysicalSegment(0, 0);
        memset(ent->inMailbox, 0, MLX_CMD_DATA_BLOCK_SIZE);
    }
    /* out mailbox (when needed) */
    if (outSize > 16) {
        ent->outMailboxDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, MLX_CMD_DATA_BLOCK_SIZE,
            0xFFFFFFF000ULL, 0);
        if (!ent->outMailboxDesc)
            return kIOReturnNoMemory;
        if (ent->outMailboxDesc->prepare(kIODirectionInOut) != kIOReturnSuccess)
            return kIOReturnNoMemory;
        ent->outMailbox = ent->outMailboxDesc->getBytesNoCopy();
        ent->outMailboxDMA = ent->outMailboxDesc->getPhysicalSegment(0, 0);
        memset(ent->outMailbox, 0, MLX_CMD_DATA_BLOCK_SIZE);
    }
    return kIOReturnSuccess;
}

void MlxCmd::freeMailbox(MlxCmdEnt *ent)
{
    if (ent->inMailboxDesc) {
        ent->inMailboxDesc->complete();
        ent->inMailboxDesc->release();
        ent->inMailboxDesc = NULL;
        ent->inMailbox = NULL;
    }
    if (ent->outMailboxDesc) {
        ent->outMailboxDesc->complete();
        ent->outMailboxDesc->release();
        ent->outMailboxDesc = NULL;
        ent->outMailbox = NULL;
    }
}

kern_return_t MlxCmd::submit(uint32_t idx, MlxCmdInOut *cmd)
{
    /* See cmd_work_handler (cmd.c:969-1056)
     * Command descriptor: MlxCmdLayout (device.h:525) */
    MlxCmdLayout *lay = (MlxCmdLayout *)((uint8_t *)fCmdBuf + (idx << fLogStride));
    MlxCmdEnt *ent = fEntArr[idx];

    /* Command header: first 16 bytes (opcode/op_mod) into in[4] */
    memcpy(lay->in, cmd->in, 16);

    /* Large input → mailbox */
    if (ent && ent->inMailbox && cmd->inSize > 16) {
        MlxCmdMailbox *mb = (MlxCmdMailbox *)ent->inMailbox;
        uint32_t dataLen = cmd->inSize - 16;
        if (dataLen > MLX_CMD_DATA_BLOCK_SIZE)
            dataLen = MLX_CMD_DATA_BLOCK_SIZE;
        memcpy(mb->data, (const uint8_t *)cmd->in + 16, dataLen);
        mb->block_num = 0;
        mb->token = ent->token;
        mb->next = 0;
        lay->in_ptr = OSSwapHostToBigInt64(ent->inMailboxDMA);
        lay->inlen = OSSwapHostToBigInt32(dataLen);
        setMailboxSignature(mb, sizeof(MlxCmdMailbox));
    } else {
        lay->in_ptr = 0;
        lay->inlen = 0;
    }

    /* Large output → mailbox */
    if (ent && ent->outMailbox && cmd->outSize > 16) {
        lay->out_ptr = OSSwapHostToBigInt64(ent->outMailboxDMA);
        lay->outlen = OSSwapHostToBigInt32(cmd->outSize - 16);
    } else {
        lay->out_ptr = 0;
        lay->outlen = 0;
    }

    /* type + token */
    lay->type = 0x1;                 /* MLX5_PCI_CMD_XPORT */
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
    OSMemoryBarrier();
    IOWrite32(fBar0, offsetof(struct MlxInitSeg, cmd_dbell), (1u << idx));

    return kIOReturnSuccess;
}

kern_return_t MlxCmd::exec(MlxCmdInOut *cmd, uint32_t timeoutMs)
{
    if (!fUp)
        return kIOReturnNotReady;

    uint32_t idx;
    if (!allocIdx(&idx))
        return kIOReturnNoSpace;

    /* Build the in-flight entity */
    MlxCmdEnt ent = {};
    ent.idx = idx;
    ent.token = (fToken++ & 0xFF);
    ent.lay = (MlxCmdLayout *)((uint8_t *)fCmdBuf + (idx << fLogStride));
    ent.done = false;
    ent.pending = true;
    fEntArr[idx] = &ent;

    /* Allocate mailbox (large command) */
    kern_return_t kr = allocMailbox(&ent, cmd->inSize, cmd->outSize);
    if (kr != kIOReturnSuccess) {
        freeIdx(idx);
        fEntArr[idx] = NULL;
        return kr;
    }

    kr = submit(idx, cmd);
    if (kr != kIOReturnSuccess) {
        freeMailbox(&ent);
        freeIdx(idx);
        fEntArr[idx] = NULL;
        return kr;
    }

    /* Wait for completion (poll the ownership bit, See poll_timeout cmd.c:237)
     * MVP uses polling; EQ interrupt event mode will be added later */
    uint64_t deadline = (uint64_t)timeoutMs * 1000ULL;
    while (!ent.done) {
        if (timeoutMs) {
            if (deadline == 0)
                break;
            deadline--;
        }
        OSMemoryBarrier();
        uint8_t own = ent.lay->status_own;
        if (!(own & MLX_CMD_OWNER_HW)) {
            ent.done = true;
        }
        if (!ent.done && !timeoutMs) {
            IOSleep(1);
        }
    }

    if (!ent.done) {
        freeMailbox(&ent);
        freeIdx(idx);
        fEntArr[idx] = NULL;
        return kIOReturnTimeout;
    }

    /* Read back the response: first 16 bytes out[4] + large output from out mailbox */
    if (cmd->out && cmd->outSize) {
        uint32_t copyLen = (cmd->outSize < 16) ? cmd->outSize : 16;
        memcpy(cmd->out, ent.lay->out, copyLen);
        if (cmd->outSize > 16 && ent.outMailbox) {
            MlxCmdMailbox *mb = (MlxCmdMailbox *)ent.outMailbox;
            uint32_t mbLen = cmd->outSize - 16;
            if (mbLen > MLX_CMD_DATA_BLOCK_SIZE)
                mbLen = MLX_CMD_DATA_BLOCK_SIZE;
            memcpy((uint8_t *)cmd->out + 16, mb->data, mbLen);
        }
    }

    /* Status: status_own >> 1 */
    uint8_t status = (ent.lay->status_own >> 1) & 0x7F;
    kern_return_t result = (status == 0) ? kIOReturnSuccess : kIOReturnIOError;
    if (result != kIOReturnSuccess) {
        IOLog("MlxCmd: opcode=%04x firmware error status=%u\n", cmd->opcode, status);
    }

    freeMailbox(&ent);
    freeIdx(idx);
    fEntArr[idx] = NULL;
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
