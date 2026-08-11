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
#include "MlxKernelCompat.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxCQ, OSObject)

void MlxCQ::free()
{
    if (fCqTable) {
        while (fCqTable->getCount()) {
            MlxCQContext *ctx = mlxRecordValue<MlxCQContext>(
                fCqTable->getObject(0));
            if (ctx) {
                uint32_t cqn = ctx->cqNumber;
                if (fRoce && fRoce->getCore() &&
                    fRoce->getCore()->getCmd() &&
                    fRoce->getCore()->getCmd()->isUp()) {
                    if (destroyCQ(cqn) == kIOReturnSuccess)
                        continue;
                }
                if (ctx->cqeDmaMap)
                    mlxUnmapDMA(ctx->cqeDmaMap);
                if (ctx->cqeBufDesc)
                    ctx->cqeBufDesc->release();
                if (fRoce && fRoce->getCore() && fRoce->getCore()->getUAR())
                    fRoce->getCore()->getUAR()->freeDbSlots(
                        ctx->dbRecordOffset, 2);
                IOFree(ctx, sizeof(*ctx));
            }
            fCqTable->removeObject(static_cast<unsigned int>(0));
        }
        fCqTable->release();
        fCqTable = NULL;
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    super::free();
}

bool MlxCQ::init(MlxRoCE *roce)
{
    if (!super::init())
        return false;
    fRoce = roce;
    fCqTable = OSArray::withCapacity(32);
    fLock = IOLockAlloc();
    return fCqTable && fLock;
}

kern_return_t MlxCQ::cmdCreateCQ(MlxCQContext *cq, uint32_t eqNumber)
{
    /* mlx5 IFC create_cq_in: CQC starts at bit 0x80 and PAS at bit 0x880. */
    uint8_t in[4096] = {};
    uint8_t out[64] = {};
    uint32_t cqcOff = 0x80 / 8;
    uint32_t pasOff = 0x880 / 8;

    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_CQ);

    uint8_t *cqc = in + cqcOff;
    mlxSetBits(cqc, 0x08, 3, 0); /* 64-byte CQE */
    mlxSetBits(cqc, 0x63, 5, cq->logSize);
    mlxSetBits(cqc, 0x68, 24,
               fRoce->getCore()->getUAR()->getBootUarIndex());
    mlxSetBits(cqc, 0xa0, 32, eqNumber);
    mlxSetBits(cqc, 0xc3, 5, 0); /* 4 KiB pages */
    mlxSetBits(cqc, 0x1c0, 64,
               fRoce->getCore()->getUAR()->getDbRecordDMA() +
                   cq->dbRecordOffset);
    for (uint32_t i = 0; i < cq->numPages; i++)
        OSWriteBigInt64(in, pasOff + i * 8, cq->pageDMA[i]);

    uint32_t inSize = pasOff + cq->numPages * 8;
    kern_return_t kr = fRoce->getCore()->exec(MLX_CMD_OP_CREATE_CQ,
                                              in, inSize, out, sizeof(out),
                                              5000);
    if (kr != kIOReturnSuccess)
        return kr;

    cq->cqNumber = static_cast<uint32_t>(mlxGetBits(out, 0x48, 24));
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

kern_return_t MlxCQ::createCQ(uint32_t entries,
                              struct mlx_create_cq_resp *resp)
{
    /* Use at least one 4 KiB CQ page (64 CQEs); 32 PAS pages cap this
     * implementation at 2048 entries. */
    if (!resp || entries == 0 || entries > 2048)
        return kIOReturnBadArgument;
    if (entries < 64)
        entries = 64;
    MlxCQContext *cq = (MlxCQContext *)IOMallocZero(sizeof(MlxCQContext));
    if (!cq)
        return kIOReturnNoMemory;

    cq->logSize = 0;
    while ((1u << cq->logSize) < entries)
        cq->logSize++;
    cq->cqeSize = sizeof(MlxCqe64);
    if (fRoce->getCore()->getUAR()->allocDbSlots(2,
                                                 &cq->dbRecordOffset) !=
        kIOReturnSuccess) {
        IOFree(cq, sizeof(MlxCQContext));
        return kIOReturnNoResources;
    }
    cq->compVector = 0;
    cq->armSn = 0;

    /* CQE buffer DMA attach: allocate a kernel DMA-coherent buffer, write its physical address to CQC PAS
     * (see create_cq_user, cq.c:717: the kernel allocates the CQ buffer) */
    uint32_t cqBytes = (1u << cq->logSize) * sizeof(MlxCqe64);
    IOBufferMemoryDescriptor *bufDesc =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut, cqBytes, 0xFFFFFFF000ULL);
    if (!bufDesc) {
        fRoce->getCore()->getUAR()->freeDbSlots(cq->dbRecordOffset, 2);
        IOFree(cq, sizeof(MlxCQContext));
        return kIOReturnNoMemory;
    }
    memset(bufDesc->getBytesNoCopy(), 0, cqBytes);
    for (uint32_t i = 0; i < (1u << cq->logSize); i++)
        reinterpret_cast<MlxCqe64 *>(bufDesc->getBytesNoCopy())[i].op_own =
            (0x0f << 4) | 1;
    cq->cqeBufAddr = (uint64_t)(uintptr_t)bufDesc->getBytesNoCopy();
    cq->cqeDmaMap = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 4096, IODMACommand::kMapped,
        cqBytes, 4096);
    if (!cq->cqeDmaMap ||
        cq->cqeDmaMap->setMemoryDescriptor(bufDesc) != kIOReturnSuccess) {
        if (cq->cqeDmaMap) cq->cqeDmaMap->release();
        cq->cqeDmaMap = NULL;
        bufDesc->release();
        fRoce->getCore()->getUAR()->freeDbSlots(cq->dbRecordOffset, 2);
        IOFree(cq, sizeof(MlxCQContext));
        return kIOReturnNoMemory;
    }
    UInt64 mapOffset = 0;
    while (mapOffset < cqBytes && cq->numPages < 32) {
        IODMACommand::Segment64 segments[8];
        UInt32 count = 8;
        if (cq->cqeDmaMap->gen64IOVMSegments(&mapOffset, segments,
                                             &count) != kIOReturnSuccess ||
            count == 0)
            break;
        for (UInt32 i = 0; i < count && cq->numPages < 32; i++) {
            if ((segments[i].fIOVMAddr & 0xfff) ||
                segments[i].fLength == 0 || segments[i].fLength > 4096) {
                mapOffset = 0;
                break;
            }
            cq->pageDMA[cq->numPages++] = segments[i].fIOVMAddr;
        }
    }
    if (mapOffset != cqBytes || cq->numPages == 0) {
        mlxUnmapDMA(cq->cqeDmaMap);
        fRoce->getCore()->getUAR()->freeDbSlots(cq->dbRecordOffset, 2);
        bufDesc->release();
        IOFree(cq, sizeof(MlxCQContext));
        return kIOReturnNoSpace;
    }
    cq->cqeDMA = cq->pageDMA[0];
    cq->cqeBufDesc = bufDesc;    /* held until destroyed */

    /* Bind to the completion EQ (vector 0) */
    uint32_t eqNumber = fRoce->getCore()->getEQ()->getCompEqNumber(0);
    kern_return_t kr = cmdCreateCQ(cq, eqNumber);
    if (kr != kIOReturnSuccess) {
        mlxUnmapDMA(cq->cqeDmaMap);
        fRoce->getCore()->getUAR()->freeDbSlots(cq->dbRecordOffset, 2);
        bufDesc->release();
        IOFree(cq, sizeof(MlxCQContext));
        return kr;
    }

    OSData *record = OSData::withBytesNoCopy(cq, sizeof(*cq));
    if (!record) {
        cmdDestroyCQ(cq->cqNumber);
        mlxUnmapDMA(cq->cqeDmaMap);
        fRoce->getCore()->getUAR()->freeDbSlots(cq->dbRecordOffset, 2);
        bufDesc->release();
        IOFree(cq, sizeof(MlxCQContext));
        return kIOReturnNoMemory;
    }
    IOLockLock(fLock);
    bool added = fCqTable->setObject(record);
    IOLockUnlock(fLock);
    record->release();
    if (!added) {
        cmdDestroyCQ(cq->cqNumber);
        mlxUnmapDMA(cq->cqeDmaMap);
        fRoce->getCore()->getUAR()->freeDbSlots(cq->dbRecordOffset, 2);
        bufDesc->release();
        IOFree(cq, sizeof(MlxCQContext));
        return kIOReturnNoMemory;
    }
    resp->cqHandle = cq->cqNumber;
    resp->logSize = cq->logSize;
    resp->cqeSize = cq->cqeSize;
    resp->dbRecordOffset = cq->dbRecordOffset;

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
            MlxCQContext *ctx = mlxRecordValue<MlxCQContext>(
                fCqTable->getObject(i));
            if (ctx && ctx->cqNumber == cqHandle) {
                /* Release the CQE buffer */
                if (ctx->cqeBufDesc) {
                    mlxUnmapDMA(ctx->cqeDmaMap);
                    ctx->cqeDmaMap = NULL;
                    ctx->cqeBufDesc->release();
                }
                fRoce->getCore()->getUAR()->freeDbSlots(
                    ctx->dbRecordOffset, 2);
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
        MlxCQContext *ctx = mlxRecordValue<MlxCQContext>(
            fCqTable->getObject(i));
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
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fCqTable->getCount(); i++) {
        MlxCQContext *ctx = mlxRecordValue<MlxCQContext>(
            fCqTable->getObject(i));
        if (!ctx || ctx->cqNumber != cqn)
            continue;
        ctx->armSn++;
        ctx->completions++;
        if (ctx->completionHandler)
            ctx->completionHandler(cqn, ctx->completionContext);
        break;
    }
    IOLockUnlock(fLock);
    /* Complete: trigger userspace polling (later in P5: event channel) */
}

IOMemoryDescriptor *MlxCQ::getCqMemDesc(uint32_t cqHandle)
{
    IOMemoryDescriptor *desc = NULL;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fCqTable->getCount(); i++) {
        MlxCQContext *ctx = mlxRecordValue<MlxCQContext>(
            fCqTable->getObject(i));
        if (ctx && ctx->cqNumber == cqHandle) {
            desc = ctx->cqeBufDesc;
            if (desc) desc->retain();
            break;
        }
    }
    IOLockUnlock(fLock);
    return desc;
}

uint64_t MlxCQ::getCompletions(uint32_t cqHandle)
{
    uint64_t completions = 0;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fCqTable->getCount(); i++) {
        MlxCQContext *ctx = mlxRecordValue<MlxCQContext>(
            fCqTable->getObject(i));
        if (ctx && ctx->cqNumber == cqHandle) {
            completions = ctx->completions;
            break;
        }
    }
    IOLockUnlock(fLock);
    return completions;
}

kern_return_t MlxCQ::updateCqConsumer(uint32_t cqHandle,
                                       uint32_t consumerIndex)
{
    /* Write the consumer index to the DB record page so hardware knows
     * which CQEs have been consumed. See mlx5_cq_set_ci (cq.c).
     * The DB record is kernel DMA-coherent memory; userspace cannot
     * write it directly (the mapping is not exposed). The write is
     * done under fLock to avoid racing with destroyCQ. */
    if (!fRoce || !fRoce->getCore() || !fRoce->getCore()->getUAR())
        return kIOReturnNotReady;
    uint32_t *db = fRoce->getCore()->getUAR()->getDbRecord();
    if (!db)
        return kIOReturnNotReady;
    kern_return_t ret = kIOReturnNotFound;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fCqTable->getCount(); i++) {
        MlxCQContext *ctx = mlxRecordValue<MlxCQContext>(
            fCqTable->getObject(i));
        if (!ctx || ctx->cqNumber != cqHandle)
            continue;
        uint32_t depth = 1u << ctx->logSize;
        if (consumerIndex > depth * 2) {
            ret = kIOReturnBadArgument;
            break;
        }
        uint32_t slot = ctx->dbRecordOffset / sizeof(uint32_t);
        db[slot] = OSSwapHostToBigInt32(consumerIndex);
        mlxMemoryBarrier();
        ret = kIOReturnSuccess;
        break;
    }
    IOLockUnlock(fLock);
    return ret;
}
