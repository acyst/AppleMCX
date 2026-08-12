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
#include <mach/vm_param.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxUAR, OSObject)

void MlxUAR::free()
{
    freeDbRecord();
    if (fBootUarValid) {
        freeUar(&fBootUar);
        fBootUarValid = false;
    }
    if (fUarPool) {
        fUarPool->release();
        fUarPool = NULL;
    }
    if (fDbLock) {
        IOLockFree(fDbLock);
        fDbLock = NULL;
    }
    super::free();
}

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
    fUarOffset = 0;
    memset(&fBootUar, 0, sizeof(fBootUar));
    fBootUarValid = false;
    fDbRecord = NULL;
    fDbRecordDMA = 0;
    fDbMemDesc = NULL;
    fDbDmaMap = NULL;
    fDbBitmap[0] = 0;
    fDbBitmap[1] = 0;
    fDbLock = IOLockAlloc();
    return fUarPool != NULL && fDbLock != NULL;
}

kern_return_t MlxUAR::allocDbSlots(uint32_t count, uint32_t *offset)
{
    if (!fDbLock || !fDbRecord || !offset || count == 0 || count > 2)
        return kIOReturnBadArgument;
    IOLockLock(fDbLock);
    for (uint32_t slot = 0; slot + count <= 128; slot++) {
        bool free = true;
        for (uint32_t i = 0; i < count; i++) {
            if (fDbBitmap[(slot + i) >> 6] &
                (1ULL << ((slot + i) & 63))) {
                free = false;
                break;
            }
        }
        if (!free)
            continue;
        for (uint32_t i = 0; i < count; i++)
            fDbBitmap[(slot + i) >> 6] |= 1ULL << ((slot + i) & 63);
        *offset = slot * sizeof(uint32_t);
        memset(reinterpret_cast<uint8_t *>(fDbRecord) + *offset, 0,
               count * sizeof(uint32_t));
        IOLockUnlock(fDbLock);
        return kIOReturnSuccess;
    }
    IOLockUnlock(fDbLock);
    return kIOReturnNoResources;
}

void MlxUAR::freeDbSlots(uint32_t offset, uint32_t count)
{
    if (!fDbLock || count == 0 || offset % sizeof(uint32_t))
        return;
    uint32_t slot = offset / sizeof(uint32_t);
    if (slot + count > 128)
        return;
    IOLockLock(fDbLock);
    for (uint32_t i = 0; i < count; i++)
        fDbBitmap[(slot + i) >> 6] &= ~(1ULL << ((slot + i) & 63));
    IOLockUnlock(fDbLock);
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
    memset(fDbMemDesc->getBytesNoCopy(), 0, 4096);
    fDbRecord = (uint32_t *)fDbMemDesc->getBytesNoCopy();
    if (mlxMapDMAContiguous(fDbMemDesc, &fDbDmaMap, &fDbRecordDMA) !=
        kIOReturnSuccess) {
        fDbMemDesc->release();
        fDbMemDesc = NULL;
        return kIOReturnNoMemory;
    }
    IOLog("MlxUAR: DB record page allocated dma=0x%llx\n", fDbRecordDMA);
    return kIOReturnSuccess;
}

void MlxUAR::freeDbRecord()
{
    if (fDbMemDesc) {
        mlxUnmapDMA(fDbDmaMap);
        fDbDmaMap = NULL;
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
    *index = static_cast<uint32_t>(mlxGetBits(out, 0x48, 24));
    return kIOReturnSuccess;
}

kern_return_t MlxUAR::cmdFreeUar(uint32_t index)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_FREE_UAR);
    mlxSetBits(in, 0x48, 24, index);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_FREE_UAR };
    return fOwner->getCmd()->exec(&cmd, 5000);
}

uint64_t MlxUAR::uarPhys(uint32_t index)
{
    /* See uar.c:70 uar2pfn:
     *   pfn = (bar_addr >> PAGE_SHIFT) + system_page_index */
    uint64_t barAddr = fOwner->getBar0()->getPhysicalAddress();
    uint64_t systemPage = fOwner->getHCA()->caps().uar4k ?
        (index >> fOwner->getHCA()->caps().logUarPageSize) : index;
    return barAddr + systemPage * PAGE_SIZE;
}

kern_return_t MlxUAR::allocUar(MlxUarAlloc *uar)
{
    uint32_t index;
    kern_return_t kr = cmdAllocUar(&index);
    if (kr != kIOReturnSuccess)
        return kr;

    uar->index = index;
    uar->phys = uarPhys(index);
    fUarOffset = fOwner->getHCA()->caps().uar4k ?
        (index & ((PAGE_SIZE / 4096) - 1)) * 4096 : 0;
    uar->wc = false;

    /* Map into the kernel address space (See ioremap, uar.c:124)
     * Use IODeviceMemory to map the UAR page within BAR0 */
    uint64_t phys = uar->phys;
    IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
        phys, PAGE_SIZE, kIODirectionInOut);
    if (desc) {
        IOMemoryMap *map = desc->createMappingInTask(
            kernel_task, 0, kIOMapAnywhere, 0, PAGE_SIZE);
        if (map) {
            uar->map = reinterpret_cast<void *>(
                static_cast<uintptr_t>(map->getVirtualAddress()));
            fUarMap = map;       /* keep the kernel mapping alive */
            fUarMemDesc = desc;   /* keep for user-space mapping */
        } else {
            desc->release();
            cmdFreeUar(index);
            return kIOReturnError;
        }
    } else {
        cmdFreeUar(index);
        return kIOReturnNoMemory;
    }

    /* The first allocated UAR becomes the boot UAR (used for EQ/CQ uar_page) */
    if (fBootIndex == 0 && fUarPool->getCount() == 0)
        fBootIndex = index;
    if (!fBootUarValid) {
        fBootUar = *uar;
        fBootUarValid = true;
    }
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
    if (!uar)
        return;
    cmdFreeUar(uar->index);
    if (fUarMap) {
        fUarMap->release();
        fUarMap = NULL;
    }
    if (fUarMemDesc) {
        fUarMemDesc->release();
        fUarMemDesc = NULL;
    }
    if (fBootUarValid && fBootUar.index == uar->index)
        fBootUarValid = false;
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

    uint32_t stride = 1u << fOwner->getHCA()->caps().logBfRegSize;
    bfreg->offset = MLX_BF_OFFSET + dbi * stride;
    if (!fUarMap || bfreg->offset + sizeof(uint64_t) > 4096) {
        fUarBitmap &= ~(1u << dbi);
        return kIOReturnNotReady;
    }
    bfreg->map = reinterpret_cast<void *>(
        static_cast<uintptr_t>(getUarVirtualAddress()) + bfreg->offset);
    IOLog("MlxUAR: BF allocated dbi=%u offset=0x%x\n", dbi, bfreg->offset);
    return kIOReturnSuccess;
}

void MlxUAR::freeBfreg(MlxBfreg *bfreg)
{
    if (!bfreg || bfreg->offset < MLX_BF_OFFSET)
        return;
    uint32_t stride = 1u << fOwner->getHCA()->caps().logBfRegSize;
    if (!stride || (bfreg->offset - MLX_BF_OFFSET) % stride)
        return;
    uint32_t dbi = (bfreg->offset - MLX_BF_OFFSET) / stride;
    if (dbi >= 4)
        return;
    fUarBitmap &= ~(1u << dbi);
}
