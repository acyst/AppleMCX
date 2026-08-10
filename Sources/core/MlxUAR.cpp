/*
 * MlxUAR.cpp — UAR/BF management implementation (generic Mellanox family)
 *
 * Ported from: mlx5_core/uar.c
 * A UAR is a 4K page within BAR0; after user-space mmap the doorbell is
 * written directly (zero syscalls).
 */
#include "MlxUAR.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSData.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxUAR, OSObject)

bool MlxUAR::init(MlxPCIDriver *owner)
{
    if (!super::init())
        return false;

    fOwner = owner;
    fUarPool = OSArray::withCapacity(4);
    fUarBitmap = 0;
    fUarMemDesc = NULL;
    fUarMap = NULL;
    fBootIndex = 0;
    fDbRecord = NULL;
    fDbRecordDMA = 0;
    fDbMemDesc = NULL;
    return true;
}

kern_return_t MlxUAR::allocDbRecord()
{
    /* DB record page (DMA-coherent, 4K): user-space post_send updates the SQ/RQ head pointers
     * See mlx5_db_alloc (driver.h:1006 struct mlx5_db)
     * Layout: db[0]=RQ DB, db[1]=SQ DB (See mlx5_db_type) */
    fDbMemDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, 4096, 0xFFFFFFF000ULL);
    if (!fDbMemDesc)
        return kIOReturnNoMemory;
    if (fDbMemDesc->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        fDbMemDesc->release();
        fDbMemDesc = NULL;
        return kIOReturnNoMemory;
    }
    memset(fDbMemDesc->getBytesNoCopy(), 0, 4096);
    fDbRecord = (uint32_t *)fDbMemDesc->getBytesNoCopy();
    fDbRecordDMA = fDbMemDesc->getPhysicalSegment(0, 0);
    IOLog("MlxUAR: DB record page allocated dma=0x%llx\n", fDbRecordDMA);
    return kIOReturnSuccess;
}

void MlxUAR::freeDbRecord()
{
    if (fDbMemDesc) {
        fDbMemDesc->complete();
        fDbMemDesc->release();
        fDbMemDesc = NULL;
        fDbRecord = NULL;
    }
}

kern_return_t MlxUAR::cmdAllocUar(uint32_t *index)
{
    /* See uar.c:38 mlx5_cmd_alloc_uar (ALLOC_UAR command) */
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_ALLOC_UAR);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_ALLOC_UAR };
    kern_return_t kr = fOwner->getCmd()->exec(&cmd, 5000);
    if (kr != kIOReturnSuccess)
        return kr;
    /* Response uar field (offset 4) */
    *index = OSReadBigInt32(out, 4) & 0xFFFFFF;
    return kIOReturnSuccess;
}

kern_return_t MlxUAR::cmdFreeUar(uint32_t index)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_FREE_UAR);
    OSWriteBigInt32(in, 4, index);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_FREE_UAR };
    return fOwner->getCmd()->exec(&cmd, 5000);
}

uint64_t MlxUAR::uarPhys(uint32_t index)
{
    /* See uar.c:70 uar2pfn:
     *   pfn = (bar_addr >> PAGE_SHIFT) + system_page_index */
    uint64_t barAddr = fOwner->getBar0()->getPhysicalAddress();
    uint64_t pageIndex = (uint64_t)index << 12;   /* 4K UAR */
    return barAddr + pageIndex;
}

kern_return_t MlxUAR::allocUar(MlxUarAlloc *uar)
{
    uint32_t index;
    kern_return_t kr = cmdAllocUar(&index);
    if (kr != kIOReturnSuccess)
        return kr;

    uar->index = index;
    uar->phys = uarPhys(index);
    uar->wc = false;

    /* Map into the kernel address space (See ioremap, uar.c:124)
     * Use IODeviceMemory to map the UAR page within BAR0 */
    uint64_t phys = uar->phys;
    IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
        phys, 4096, kIODirectionInOut);
    if (desc) {
        IOMemoryMap *map = desc->createMappingInTask(
            kernel_task, 0, kIOMapAnywhere, 0, 4096);
        if (map) {
            uar->map = reinterpret_cast<void *>(
                static_cast<uintptr_t>(map->getVirtualAddress()));
            fUarMap = map;       /* keep the kernel mapping alive */
            fUarMemDesc = desc;   /* keep for user-space mapping */
        } else {
            desc->release();
            return kIOReturnError;
        }
    } else {
        return kIOReturnNoMemory;
    }

    /* The first allocated UAR becomes the boot UAR (used for EQ/CQ uar_page) */
    if (fBootIndex == 0 && fUarPool->getCount() == 0)
        fBootIndex = index;
    /* Pool record (MVP: record the virtual address for reuse) */
    OSData *record = OSData::withBytes(uar, sizeof(MlxUarAlloc));
    if (record) {
        fUarPool->setObject(record);
        record->release();
    }

    IOLog("MlxUAR: UAR[%u] allocated phys=0x%llx map=%p\n",
          index, uar->phys, uar->map);
    return kIOReturnSuccess;
}

void MlxUAR::freeUar(MlxUarAlloc *uar)
{
    cmdFreeUar(uar->index);
    if (fUarMap) {
        fUarMap->release();
        fUarMap = NULL;
    }
    if (fUarMemDesc) {
        fUarMemDesc->release();
        fUarMemDesc = NULL;
    }
}

kern_return_t MlxUAR::allocBfreg(MlxBfreg *bfreg)
{
    /* See uar.c:258 mlx5_alloc_bfreg
     * BF offset = MLX_BF_OFFSET(0x800) + (dbi%4) * BF size */
    uint32_t dbi = 0;
    uint32_t free = (~fUarBitmap) & 0xF;
    if (!free)
        return kIOReturnNoSpace;
    dbi = __builtin_ctz(free);
    fUarBitmap |= (1u << dbi);

    bfreg->offset = MLX_BF_OFFSET + (dbi << 12);
    if (!fUarMap)
        return kIOReturnNotReady;
    bfreg->map = reinterpret_cast<void *>(
        static_cast<uintptr_t>(fUarMap->getVirtualAddress()) + bfreg->offset);
    IOLog("MlxUAR: BF allocated dbi=%u offset=0x%x\n", dbi, bfreg->offset);
    return kIOReturnSuccess;
}

void MlxUAR::freeBfreg(MlxBfreg *bfreg)
{
    uint32_t dbi = (bfreg->offset - MLX_BF_OFFSET) >> 12;
    fUarBitmap &= ~(1u << dbi);
}
