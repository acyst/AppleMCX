/*
 * MlxCQ.cpp — Completion Queue implementation (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/cq.c (create_cq_user, cq.c:717)
 * Trimmed: create/destroy; poll_cq reads the CQE buffer directly from userspace
 */
#include "MlxCQ.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"
#include "MlxEQ.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxCQ, OSObject)

bool MlxCQ::init(MlxRoCE *roce)
{
    if (!super::init())
        return false;
    fRoce = roce;
    fCqTable = OSArray::withCapacity(32);
    fLock = IOLockAlloc();
    return true;
}

kern_return_t MlxCQ::cmdCreateCQ(MlxCQContext *cq, uint32_t eqNumber)
{
    /* See create_cq_user (cq.c:717)
     * create_cq_in = 64B header + cqc(128B) + event_bitmask(128B) + pas */
    uint8_t in[4096] = {};
    uint8_t out[64] = {};
    uint32_t cqcOff = 0x40;
    uint32_t maskOff = cqcOff + 128;
    uint32_t pasOff = maskOff + 128;

    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_CQ);

    /* cqc (see mlx5_ifc.h cqc_bits):
     *   cqe_sz(4bit) at +0x08, log_cq_size(5bit) at +0x20+3,
     *   uar_page(24bit) at +0x30, eqn(24bit) at +0x40, log_page_size at +0x80 */
    uint8_t *cqc = in + cqcOff;
    cqc[0x08] = 0x0;                            /* cqe_sz = 0 (64B) */
    cqc[0x20] = (uint8_t)((cq->logSize << 3) & 0xF8);   /* log_cq_size */
    cqc[0x30] = 0;                              /* uar_page = 0 */
    cqc[0x40] = (uint8_t)((eqNumber >> 16) & 0xFF);
    cqc[0x41] = (uint8_t)((eqNumber >> 8) & 0xFF);
    cqc[0x42] = (uint8_t)(eqNumber & 0xFF);

    /* event mask: completion events */
    OSWriteBigInt32(in, maskOff, 1u << MLX_EVENT_TYPE_COMPLETION);

    /* PAS */
    OSWriteBigInt64(in, pasOff, cq->cqeDMA);

    uint32_t inSize = pasOff + 8;
    MlxCmdInOut cmd = { in, inSize, out, sizeof(out), MLX_CMD_OP_CREATE_CQ };
    kern_return_t kr = fRoce->getCore()->exec(MLX_CMD_OP_CREATE_CQ,
                                              in, inSize,
                                              out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        return kr;

    /* create_cq_out: cqn at +0x40+16 */
    cq->cqNumber = OSReadBigInt32(out, 0x50) & 0xFFFFFF;
    return kIOReturnSuccess;
}

kern_return_t MlxCQ::cmdDestroyCQ(uint32_t cqNumber)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_CQ);
    OSWriteBigInt32(in, 4, cqNumber);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_DESTROY_CQ };
    return fRoce->getCore()->exec(MLX_CMD_OP_DESTROY_CQ, in, sizeof(in),
                                  out, sizeof(out), 5000);
}

kern_return_t MlxCQ::createCQ(uint32_t cqeSize, uint32_t *cqHandle)
{
    MlxCQContext *cq = (MlxCQContext *)IOMallocZero(sizeof(MlxCQContext));
    if (!cq)
        return kIOReturnNoMemory;

    cq->logSize = 8;            /* 256 entries */
    cq->cqeSize = cqeSize ? cqeSize : 64;
    cq->dbRecordOffset = 0;
    cq->compVector = 0;
    cq->armSn = 0;

    /* CQE buffer DMA attach: allocate a kernel DMA-coherent buffer, write its physical address to CQC PAS
     * (see create_cq_user, cq.c:717: the kernel allocates the CQ buffer) */
    uint32_t cqBytes = (1u << cq->logSize) * cq->cqeSize;
    IOBufferMemoryDescriptor *bufDesc =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, cqBytes, 0xFFFFFFF000ULL, 0);
    if (!bufDesc) {
        IOFree(cq, sizeof(MlxCQContext));
        return kIOReturnNoMemory;
    }
    if (bufDesc->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        bufDesc->release();
        IOFree(cq, sizeof(MlxCQContext));
        return kIOReturnNoMemory;
    }
    memset(bufDesc->getBytesNoCopy(), 0, cqBytes);
    cq->cqeBufAddr = (uint64_t)(uintptr_t)bufDesc->getBytesNoCopy();
    cq->cqeDMA = bufDesc->getPhysicalSegment(0, 0);
    cq->cqeBufDesc = bufDesc;    /* held until destroyed */

    /* Bind to the completion EQ (vector 0) */
    uint32_t eqNumber = fRoce->getCore()->getEQ()->getCompEqNumber(0);
    kern_return_t kr = cmdCreateCQ(cq, eqNumber);
    if (kr != kIOReturnSuccess) {
        bufDesc->complete();
        bufDesc->release();
        IOFree(cq, sizeof(MlxCQContext));
        return kr;
    }

    IOLockLock(fLock);
    fCqTable->setObject(cq);
    IOLockUnlock(fLock);
    *cqHandle = cq->cqNumber;

    IOLog("MlxCQ: CQ[%u] created log_size=%u eqn=%u cqe_dma=0x%llx\n",
          cq->cqNumber, cq->logSize, eqNumber, cq->cqeDMA);
    return kIOReturnSuccess;
}

kern_return_t MlxCQ::destroyCQ(uint32_t cqHandle)
{
    kern_return_t kr = cmdDestroyCQ(cqHandle);
    if (kr == kIOReturnSuccess) {
        IOLockLock(fLock);
        for (uint32_t i = 0; i < fCqTable->getCount(); i++) {
            MlxCQContext *ctx = (MlxCQContext *)fCqTable->getObject(i);
            if (ctx && ctx->cqNumber == cqHandle) {
                /* Release the CQE buffer */
                if (ctx->cqeBufDesc) {
                    ctx->cqeBufDesc->complete();
                    ctx->cqeBufDesc->release();
                }
                fCqTable->removeObject(i);
                IOFree(ctx, sizeof(MlxCQContext));
                break;
            }
        }
        IOLockUnlock(fLock);
    }
    return kr;
}

MlxCQContext *MlxCQ::lookup(uint32_t cqHandle)
{
    MlxCQContext *found = NULL;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fCqTable->getCount(); i++) {
        MlxCQContext *ctx = (MlxCQContext *)fCqTable->getObject(i);
        if (ctx && ctx->cqNumber == cqHandle) {
            found = ctx;
            break;
        }
    }
    IOLockUnlock(fLock);
    return found;
}

void MlxCQ::handleCompletion(uint32_t cqn)
{
    /* See eq.c:106 mlx5_eq_comp_int + cq.c:41 mlx5_ib_cq_comp:
     *   look up the CQ by cqn → call the completion callback (comp_handler) */
    MlxCQContext *ctx = lookup(cqn);
    if (!ctx)
        return;

    ctx->armSn++;
    ctx->completions++;
    if (ctx->completionHandler)
        ctx->completionHandler(cqn, ctx->completionContext);
    /* Complete: trigger userspace polling (later in P5: event channel) */
}

IOMemoryDescriptor *MlxCQ::getCqMemDesc(uint32_t cqHandle)
{
    MlxCQContext *ctx = lookup(cqHandle);
    return ctx ? ctx->cqeBufDesc : NULL;
}

uint64_t MlxCQ::getCompletions(uint32_t cqHandle)
{
    MlxCQContext *ctx = lookup(cqHandle);
    return ctx ? ctx->completions : 0;
}
