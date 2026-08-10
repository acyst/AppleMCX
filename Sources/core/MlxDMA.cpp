/*
 * MlxDMA.cpp — DMA mapping utility implementation (generic Mellanox family)
 *
 * The key to DMA for the driver: pin user virtual memory to physical addresses,
 * used by the PAS (physical address segments) of QPC/CQC and by user-space post_send.
 *
 * See: mlx5_core's ib_umem_get (pin) + IODMACommand physical segment enumeration
 */
#include "MlxDMA.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxKernelCompat.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxDMA, OSObject)

void MlxDMA::free()
{
    if (fPendingReqs) {
        while (fPendingReqs->getCount()) {
            MlxDMAReq *req = mlxRecordValue<MlxDMAReq>(
                fPendingReqs->getObject(0));
            if (req) {
                if (req->dmaCommand) {
                    req->dmaCommand->clearMemoryDescriptor();
                    req->dmaCommand->release();
                }
                if (req->memDesc)
                    req->memDesc->release();
            }
            fPendingReqs->removeObject(static_cast<unsigned int>(0));
        }
        fPendingReqs->release();
        fPendingReqs = NULL;
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    super::free();
}

bool MlxDMA::init()
{
    if (!super::init())
        return false;
    fPendingReqs = NULL;
    fLock = NULL;
    fPendingReqs = OSArray::withCapacity(16);
    fLock = IOLockAlloc();
    return fPendingReqs && fLock;
}

kern_return_t MlxDMA::getSegments(IOMemoryDescriptor *mem,
                                  uint64_t *paList, uint32_t *numSegs)
{
    /* Enumerate physical segments (See the PBL construction of mr.c:1097 reg_create)
     * Use IODMACommand to handle possibly multi-segment physical memory */
    IODMACommand *cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped, 0, 1);
    if (!cmd)
        return kIOReturnNoMemory;
    IOReturn mapKr = cmd->setMemoryDescriptor(mem);
    if (mapKr != kIOReturnSuccess) {
        cmd->release();
        return mapKr;
    }

    uint32_t n = 0;
    UInt64 offset = 0;
    while (offset < mem->getLength() && n < MLX_DMA_MAX_SEGS) {
        IODMACommand::Segment64 segs[8];
        UInt32 numSeg = 8;
        kern_return_t kr = cmd->gen64IOVMSegments(&offset, segs, &numSeg);
        if (kr != kIOReturnSuccess || numSeg == 0)
            break;
        for (UInt32 i = 0; i < numSeg && n < MLX_DMA_MAX_SEGS; i++) {
            paList[n++] = segs[i].fIOVMAddr;
        }
    }
    cmd->clearMemoryDescriptor();
    cmd->release();

    *numSegs = n;
    return (n > 0) ? kIOReturnSuccess : kIOReturnIOError;
}

kern_return_t MlxDMA::pinUserMemory(uint64_t virtAddr, uint64_t length,
                                    MlxDMAReq *req)
{
    if (!req || length == 0)
        return kIOReturnBadArgument;

    memset(req, 0, sizeof(MlxDMAReq));
    req->ownerTask = current_task();
    req->virtAddr = virtAddr;
    req->length = length;

    /* Bind the virtual range to the calling user process. */
    IOMemoryDescriptor *mem = IOMemoryDescriptor::withAddressRange(
        virtAddr, length, kIODirectionInOut, current_task());
    if (!mem)
        return kIOReturnNoMemory;

    /* Enumerate physical segments + segment lengths */
    uint64_t paList[MLX_DMA_MAX_SEGS];
    uint64_t paLen[MLX_DMA_MAX_SEGS];
    uint32_t numSegs = 0;

    IODMACommand *cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped, 0, 1);
    if (!cmd) {
        mem->release();
        return kIOReturnNoMemory;
    }
    IOReturn kr = cmd->setMemoryDescriptor(mem);
    if (kr != kIOReturnSuccess) {
        cmd->release();
        mem->release();
        return kr;
    }

    UInt64 offset = 0;
    while (offset < length && numSegs < MLX_DMA_MAX_SEGS) {
        UInt64 segOffset = offset;
        IODMACommand::Segment64 segs[8];
        UInt32 numSeg = 8;
        kern_return_t k2 = cmd->gen64IOVMSegments(&segOffset, segs, &numSeg);
        if (k2 != kIOReturnSuccess || numSeg == 0)
            break;
        for (UInt32 i = 0; i < numSeg && numSegs < MLX_DMA_MAX_SEGS; i++) {
            paList[numSegs] = segs[i].fIOVMAddr;
            paLen[numSegs] = segs[i].fLength;
            offset += segs[i].fLength;
            numSegs++;
        }
    }
    if (numSegs == 0 || offset != length) {
        cmd->clearMemoryDescriptor();
        cmd->release();
        mem->release();
        return (numSegs == MLX_DMA_MAX_SEGS) ? kIOReturnNoSpace :
                                               kIOReturnIOError;
    }

    memcpy(req->paList, paList, sizeof(uint64_t) * numSegs);
    memcpy(req->paLenList, paLen, sizeof(uint64_t) * numSegs);
    req->numSegs = numSegs;
    req->memDesc = mem;   /* held until unpin */
    req->dmaCommand = cmd;

    OSData *record = OSData::withBytes(req, sizeof(*req));
    if (!record) {
        cmd->clearMemoryDescriptor();
        cmd->release();
        mem->release();
        req->memDesc = NULL;
        req->dmaCommand = NULL;
        return kIOReturnNoMemory;
    }
    IOLockLock(fLock);
    bool added = fPendingReqs->setObject(record);
    IOLockUnlock(fLock);
    record->release();
    if (!added) {
        cmd->clearMemoryDescriptor();
        cmd->release();
        mem->release();
        req->memDesc = NULL;
        req->dmaCommand = NULL;
        return kIOReturnNoMemory;
    }

    IOLog("MlxDMA: pin 0x%llx len=%llu segs=%u pa0=0x%llx\n",
          virtAddr, length, numSegs, paList[0]);
    return kIOReturnSuccess;
}

void MlxDMA::unpinMemory(MlxDMAReq *req)
{
    if (!req)
        return;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fPendingReqs->getCount(); i++) {
        MlxDMAReq *active = mlxRecordValue<MlxDMAReq>(
            fPendingReqs->getObject(i));
        if (active && active->memDesc == req->memDesc) {
            fPendingReqs->removeObject(i);
            break;
        }
    }
    IOLockUnlock(fLock);

    if (req->dmaCommand) {
        req->dmaCommand->clearMemoryDescriptor();
        req->dmaCommand->release();
        req->dmaCommand = NULL;
    }
    if (req->memDesc) {
        req->memDesc->release();
        req->memDesc = NULL;
    }
}

uint64_t MlxDMA::lookupPhys(task_t ownerTask, uint64_t virtAddr)
{
    /* Lookup: virtual address → physical address (for user-space post_send data segments)
     * Exact algorithm: accumulate across physical segments to locate the segment and in-segment offset of virtAddr */
    uint64_t phys = 0;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fPendingReqs->getCount(); i++) {
        MlxDMAReq *req = mlxRecordValue<MlxDMAReq>(
            fPendingReqs->getObject(i));
        if (!req || req->ownerTask != ownerTask || virtAddr < req->virtAddr)
            continue;

        uint64_t offset = virtAddr - req->virtAddr;
        if (offset >= req->length)
            continue;
        uint64_t acc = 0;
        for (uint32_t s = 0; s < req->numSegs; s++) {
            uint64_t segLen = req->paLenList[s];
            if (offset < acc + segLen) {
                /* Target segment: pa[s] + (offset - acc) */
                phys = req->paList[s] + (offset - acc);
                break;
            }
            acc += segLen;
        }
        break;
    }
    IOLockUnlock(fLock);
    return phys;
}
