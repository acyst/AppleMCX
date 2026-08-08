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

#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxMR, OSObject)

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
    return true;
}

kern_return_t MlxMR::buildPBL(IOMemoryDescriptor *mem, uint64_t *paList,
                              uint32_t *numSegs)
{
    /* Iterate over physical segments to fill the PBL (see mr.c reg_create: ib_umem → MTT) */
    uint32_t n = 0;
    IOByteCount offset = 0;
    while (offset < mem->getLength()) {
        IODMACommand *cmd = IODMACommand::withSpecification(
            kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped, 0, 1);
        if (!cmd)
            break;
        cmd->setMemoryDescriptor(mem);
        UInt64 segOffset = offset;
        IODMACommand::Segment64 segs[8];
        UInt32 numSeg = 8;
        kern_return_t kr = cmd->gen64IOVMSegments(&segOffset, segs, &numSeg);
        cmd->clearMemoryDescriptor();
        cmd->release();
        if (kr != kIOReturnSuccess)
            break;
        for (UInt32 i = 0; i < numSeg && n < MLX_MAX_MR_SEGMENTS; i++) {
            paList[n++] = segs[i].fIOVMAddr;
            offset += segs[i].fLength;
        }
        if (n >= MLX_MAX_MR_SEGMENTS)
            break;
    }
    *numSegs = n;
    return (n > 0) ? kIOReturnSuccess : kIOReturnIOError;
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

    /* create_mkey_out: mkey_index at +0x48 (24bit) */
    uint32_t mkeyIndex = OSReadBigInt32(out, 0x48) & 0xFFFFFF;
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
    OSWriteBigInt32(in, 4, mkey);
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
    IOMemoryDescriptor *mem = IOMemoryDescriptor::withTask(
        kernel_task, req->startAddr, req->length,
        (req->accessFlags & (MLX_MKC_ACCESS_LW | MLX_MKC_ACCESS_RW)) ?
            kIODirectionInOut : kIODirectionIn, NULL);
    if (!mem)
        return kIOReturnNoMemory;
    if (mem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        mem->release();
        return kIOReturnIOError;
    }

    /* Build the PBL */
    uint64_t paList[MLX_MAX_MR_SEGMENTS];
    uint32_t numSegs = 0;
    kern_return_t kr = buildPBL(mem, paList, &numSegs);
    if (kr != kIOReturnSuccess) {
        mem->complete();
        mem->release();
        return kr;
    }

    /* Issue CREATE_MKEY */
    uint32_t mkey, lkey, rkey;
    kr = cmdCreateMKey(paList, numSegs, req->startAddr, req->length,
                       req->accessFlags, req->pd,
                       &mkey, &lkey, &rkey);
    if (kr != kIOReturnSuccess) {
        mem->complete();
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
        IOLockLock(fLock);
        fMrTable->setObject(ctx);
        IOLockUnlock(fLock);
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
            MlxMRContext *ctx = (MlxMRContext *)fMrTable->getObject(i);
            if (ctx && ctx->mrHandle == mrHandle) {
                if (ctx->fMemDesc) {
                    ctx->fMemDesc->complete();
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
        MlxMRContext *ctx = (MlxMRContext *)fMrTable->getObject(i);
        if (ctx && ctx->mrHandle == mrHandle) {
            found = ctx;
            break;
        }
    }
    IOLockUnlock(fLock);
    return found;
}
