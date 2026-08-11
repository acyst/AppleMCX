/*
 * MlxMR.cpp — Memory Registration implementation (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/mr.c (reg_create, mr.c:1097)
 * Flow: pin user memory → build MTT (PBL) → CREATE_MKEY command
 * MKC layout: see mlx5_ifc.h:4116 (mkc_bits)
 */
#include "MlxMR.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxMR, OSObject)

void MlxMR::free()
{
    if (fMrTable) {
        while (fMrTable->getCount()) {
            MlxMRContext *ctx = mlxRecordValue<MlxMRContext>(
                fMrTable->getObject(0));
            if (ctx) {
                uint32_t mkey = ctx->mrHandle;
                if (fRoce && fRoce->getCore() &&
                    fRoce->getCore()->getCmd() &&
                    fRoce->getCore()->getCmd()->isUp()) {
                    if (deregMR(mkey) == kIOReturnSuccess)
                        continue;
                }
                if (ctx->fDmaCommand)
                    mlxUnmapDMA(ctx->fDmaCommand);
                if (ctx->fMemDesc)
                    ctx->fMemDesc->release();
                IOFree(ctx, sizeof(*ctx));
            }
            fMrTable->removeObject(static_cast<unsigned int>(0));
        }
        fMrTable->release();
        fMrTable = NULL;
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    super::free();
}

/* Access permission bits (see mr.c set_mkc_access_pd_addr_fields) */
#define MLX_MKC_ACCESS_LR      (1u << 0)   /* local read */
#define MLX_MKC_ACCESS_LW      (1u << 1)   /* local write */
#define MLX_MKC_ACCESS_RR      (1u << 2)   /* remote read */
#define MLX_MKC_ACCESS_RW      (1u << 3)   /* remote write */
#define MLX_MKC_ACCESS_ATOMIC  (1u << 6)

bool MlxMR::init(MlxRoCE *roce)
{
    if (!super::init())
        return false;
    fRoce = roce;
    fCore = roce->getCore();
    fMrTable = OSArray::withCapacity(16);
    fLock = IOLockAlloc();
    return fMrTable && fLock;
}

kern_return_t MlxMR::buildPBL(IOMemoryDescriptor *mem, uint64_t *paList,
                              uint32_t *numSegs, IODMACommand **dmaCommand)
{
    if (!mem || !paList || !numSegs || !dmaCommand)
        return kIOReturnBadArgument;
    *dmaCommand = NULL;
    uint32_t n = 0;
    IODMACommand *cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 4096, IODMACommand::kMapped,
        mem->getLength(), 4096);
    if (!cmd)
        return kIOReturnNoMemory;
    IOReturn kr = cmd->setMemoryDescriptor(mem);
    if (kr != kIOReturnSuccess) {
        cmd->release();
        return kr;
    }

    UInt64 offset = 0;
    while (offset < mem->getLength() && n < MLX_MAX_MR_SEGMENTS) {
        IODMACommand::Segment64 segs[8];
        UInt32 numSeg = 8;
        kr = cmd->gen64IOVMSegments(&offset, segs, &numSeg);
        if (kr != kIOReturnSuccess || numSeg == 0)
            break;
        for (UInt32 i = 0; i < numSeg && n < MLX_MAX_MR_SEGMENTS; i++) {
            if ((segs[i].fIOVMAddr & 0xfff) || segs[i].fLength != 4096)
                break;
            paList[n++] = segs[i].fIOVMAddr;
        }
    }
    if (offset != mem->getLength()) {
        cmd->clearMemoryDescriptor();
        cmd->release();
        return kIOReturnNoSpace;
    }
    *numSegs = n;
    if (!n) {
        cmd->clearMemoryDescriptor();
        cmd->release();
        return kIOReturnIOError;
    }
    *dmaCommand = cmd;
    return kIOReturnSuccess;
}

kern_return_t MlxMR::cmdCreateMKey(const uint64_t *paList, uint32_t numSegs,
                                   uint64_t startAddr, uint64_t length,
                                   uint32_t accessFlags, uint32_t pd,
                                   uint32_t *mkey, uint32_t *lkey,
                                   uint32_t *rkey)
{
    /* See create_mkey_in (mlx5_ifc.h:9097):
     * 64B header + pg_access + mkc(192B) + translations_octword_actual_size + pas */
    uint8_t in[4096] = {};
    uint8_t out[64] = {};
    uint32_t mkcOff = 0x40 + 0x20;          /* header 64 + pg_access area 32B */
    uint32_t pasOff = mkcOff + 192 + 0x80 + 4;  /* mkc + reserved + actual_size */

    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_MKEY);
    OSWriteBigInt32(in, 0x40, 0);           /* pg_access = 0 */

    /* MKC (see mkc_bits) */
    uint8_t *mkc = in + mkcOff;
    /* access permissions: lr/lw/rr/rw (low 4 bits) */
    uint8_t acc = 0;
    if (accessFlags & MLX_MKC_ACCESS_LR) acc |= 0x1;
    if (accessFlags & MLX_MKC_ACCESS_LW) acc |= 0x2;
    if (accessFlags & MLX_MKC_ACCESS_RR) acc |= 0x4;
    if (accessFlags & MLX_MKC_ACCESS_RW) acc |= 0x8;
    mkc[0x00] = acc;                        /* low byte: access bits */
    /* access_mode_1_0 (2bit) at +0x10: 0x3 = MTT */
    mkc[0x10] = (uint8_t)((mkc[0x10] & 0xFC) | 0x3);
    /* pd (24bit) at +0x44 */
    mkc[0x44] = (uint8_t)((pd >> 16) & 0xFF);
    mkc[0x45] = (uint8_t)((pd >> 8) & 0xFF);
    mkc[0x46] = (uint8_t)(pd & 0xFF);
    /* start_addr (64bit) at +0x48 */
    OSWriteBigInt64(mkc, 0x48, startAddr);
    /* len (64bit) at +0x50 */
    OSWriteBigInt64(mkc, 0x50, length);
    /* translations_octword_size (32bit) at +0x120 */
    uint32_t xltOct = numSegs * 2;          /* each MTT is 2 octwords (16B) */
    OSWriteBigInt32(mkc, 0x120, xltOct);
    /* log_page_size (5bit) at +0x1E8 */
    mkc[0x1E8] = 12;                        /* 4K page */

    /* PAS (physical address list) */
    for (uint32_t i = 0; i < numSegs; i++)
        OSWriteBigInt64(in, pasOff + (i * 8), paList[i]);

    uint32_t inSize = pasOff + (numSegs * 8);
    MlxCmdInOut cmd = { in, inSize, out, sizeof(out), MLX_CMD_OP_CREATE_MKEY };
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_MKEY, in, inSize,
                                   out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        return kr;

    uint32_t mkeyIndex = static_cast<uint32_t>(mlxGetBits(out, 0x48, 24));
    *mkey = mkeyIndex;
    /* lkey = mkey_index (low 24 bits), rkey = mkey */
    *lkey = mkeyIndex;
    *rkey = mkeyIndex;
    return kIOReturnSuccess;
}

kern_return_t MlxMR::cmdDestroyMKey(uint32_t mkey)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_MKEY);
    mlxSetBits(in, 0x48, 24, mkey);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_DESTROY_MKEY };
    return fCore->exec(MLX_CMD_OP_DESTROY_MKEY, in, sizeof(in),
                       out, sizeof(out), 5000);
}

kern_return_t MlxMR::regMR(const struct mlx_reg_mr_req *req,
                           struct mlx_reg_mr_resp *resp)
{
    if (!req || !resp)
        return kIOReturnBadArgument;

    /* Pin user memory (see ib_umem_get, mr.c) */
    IOMemoryDescriptor *mem = IOMemoryDescriptor::withAddressRange(
        req->startAddr, req->length,
        (req->accessFlags & (MLX_MKC_ACCESS_LW | MLX_MKC_ACCESS_RW)) ?
            kIODirectionInOut : kIODirectionIn,
        current_task());
    if (!mem)
        return kIOReturnNoMemory;
    /* Build the PBL */
    uint64_t paList[MLX_MAX_MR_SEGMENTS];
    uint32_t numSegs = 0;
    IODMACommand *dmaCommand = NULL;
    kern_return_t kr = buildPBL(mem, paList, &numSegs, &dmaCommand);
    if (kr != kIOReturnSuccess) {
        mem->release();
        return kr;
    }

    /* Issue CREATE_MKEY */
    uint32_t mkey, lkey, rkey;
    kr = cmdCreateMKey(paList, numSegs, req->startAddr, req->length,
                       req->accessFlags, req->pd,
                       &mkey, &lkey, &rkey);
    if (kr != kIOReturnSuccess) {
        dmaCommand->clearMemoryDescriptor();
        dmaCommand->release();
        mem->release();
        return kr;
    }

    /* Record the MR */
    MlxMRContext *ctx = (MlxMRContext *)IOMallocZero(sizeof(MlxMRContext));
    if (ctx) {
        ctx->mrHandle = mkey;
        ctx->pd = req->pd;
        ctx->lkey = lkey;
        ctx->rkey = rkey;
        ctx->startAddr = req->startAddr;
        ctx->length = req->length;
        ctx->accessFlags = req->accessFlags;
        ctx->fMemDesc = mem;
        ctx->fDmaCommand = dmaCommand;
        OSData *record = OSData::withBytesNoCopy(ctx, sizeof(*ctx));
        if (!record) {
            IOFree(ctx, sizeof(MlxMRContext));
            cmdDestroyMKey(mkey);
            dmaCommand->clearMemoryDescriptor();
            dmaCommand->release();
            mem->release();
            return kIOReturnNoMemory;
        }
        IOLockLock(fLock);
        bool added = fMrTable->setObject(record);
        IOLockUnlock(fLock);
        record->release();
        if (!added) {
            IOFree(ctx, sizeof(MlxMRContext));
            cmdDestroyMKey(mkey);
            dmaCommand->clearMemoryDescriptor();
            dmaCommand->release();
            mem->release();
            return kIOReturnNoMemory;
        }
    } else {
        cmdDestroyMKey(mkey);
        dmaCommand->clearMemoryDescriptor();
        dmaCommand->release();
        mem->release();
        return kIOReturnNoMemory;
    }

    resp->mrHandle = mkey;
    resp->lkey = lkey;
    resp->rkey = rkey;

    IOLog("MlxMR: MR[%u] registered len=%llu segs=%u lkey=%x rkey=%x\n",
          mkey, req->length, numSegs, lkey, rkey);
    return kIOReturnSuccess;
}

kern_return_t MlxMR::deregMR(uint32_t mrHandle)
{
    kern_return_t kr = cmdDestroyMKey(mrHandle);
    if (kr == kIOReturnSuccess) {
        IOLockLock(fLock);
        for (uint32_t i = 0; i < fMrTable->getCount(); i++) {
            MlxMRContext *ctx = mlxRecordValue<MlxMRContext>(
                fMrTable->getObject(i));
            if (ctx && ctx->mrHandle == mrHandle) {
                if (ctx->fMemDesc) {
                    if (ctx->fDmaCommand) {
                        ctx->fDmaCommand->clearMemoryDescriptor();
                        ctx->fDmaCommand->release();
                    }
                    ctx->fMemDesc->release();
                }
                fMrTable->removeObject(i);
                IOFree(ctx, sizeof(MlxMRContext));
                break;
            }
        }
        IOLockUnlock(fLock);
    }
    return kr;
}

MlxMRContext *MlxMR::lookup(uint32_t mrHandle)
{
    MlxMRContext *found = NULL;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fMrTable->getCount(); i++) {
        MlxMRContext *ctx = mlxRecordValue<MlxMRContext>(
            fMrTable->getObject(i));
        if (ctx && ctx->mrHandle == mrHandle) {
            found = ctx;
            break;
        }
    }
    IOLockUnlock(fLock);
    return found;
}
