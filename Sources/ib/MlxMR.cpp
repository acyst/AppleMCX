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
#include "MlxP0Encoding.hpp"

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
                IOLog("MlxMR: quarantining MKey[%x] DMA after unverified destroy\n",
                      mkey);
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

bool MlxMR::init(MlxRoCE *roce)
{
    if (!super::init())
        return false;
    fRoce = roce;
    fCore = roce->getCore();
    fMrTable = OSArray::withCapacity(16);
    fLock = IOLockAlloc();
    fMkeyVariant = 0;
    return fMrTable && fLock;
}

kern_return_t MlxMR::buildPBL(IOMemoryDescriptor *mem, uint64_t startAddr,
                              uint64_t length, uint64_t *paList,
                              uint32_t *numSegs, IODMACommand **dmaCommand)
{
    if (!mem || !length || !paList || !numSegs || !dmaCommand ||
        length != mem->getLength())
        return kIOReturnBadArgument;
    *dmaCommand = NULL;
    uint32_t n = 0;
    IODMACommand *cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped,
        mem->getLength(), 1);
    if (!cmd)
        return kIOReturnNoMemory;
    IOReturn kr = cmd->setMemoryDescriptor(mem);
    if (kr != kIOReturnSuccess) {
        cmd->release();
        return kr;
    }

    UInt64 offset = 0;
    while (offset < mem->getLength() && n < MLX_MAX_MR_SEGMENTS) {
        UInt64 batchOffset = offset;
        IODMACommand::Segment64 segs[8];
        UInt32 numSeg = 8;
        kr = cmd->gen64IOVMSegments(&offset, segs, &numSeg);
        if (kr != kIOReturnSuccess || numSeg == 0)
            break;
        for (UInt32 i = 0; i < numSeg && n < MLX_MAX_MR_SEGMENTS; i++) {
            if ((segs[i].fIOVMAddr & (MLX_MTT_PAGE_SIZE - 1)) !=
                ((startAddr + batchOffset) & (MLX_MTT_PAGE_SIZE - 1))) {
                kr = kIOReturnUnsupported;
                break;
            }
            if (!mlxAppendMttPages(segs[i].fIOVMAddr, segs[i].fLength,
                                   paList, MLX_MAX_MR_SEGMENTS, &n)) {
                kr = kIOReturnNoSpace;
                break;
            }
            batchOffset += segs[i].fLength;
        }
        if (kr != kIOReturnSuccess)
            break;
    }
    uint64_t expectedPages = mlxMttPageCount(startAddr, length);
    if (kr != kIOReturnSuccess || offset != mem->getLength() ||
        n != expectedPages) {
        cmd->clearMemoryDescriptor();
        cmd->release();
        return (n >= MLX_MAX_MR_SEGMENTS || expectedPages > MLX_MAX_MR_SEGMENTS) ?
                   kIOReturnNoSpace : kIOReturnIOError;
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
    /* create_mkey_in has a 272-byte fixed area followed by 8-byte MTTs,
     * padded to a 16-byte octword. */
    uint8_t *in = static_cast<uint8_t *>(IOMallocZero(MLX_CMD_MAX_SIZE));
    uint8_t out[64] = {};
    if (!in)
        return kIOReturnNoMemory;

    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_MKEY);
    IOLockLock(fLock);
    uint8_t variant = ++fMkeyVariant;
    if (!variant)
        variant = ++fMkeyVariant;
    IOLockUnlock(fLock);
    uint32_t inSize = 0;
    if (!mlxEncodeCreateMkey(in, MLX_CMD_MAX_SIZE, paList, numSegs,
                            startAddr, length, accessFlags, pd, variant,
                            &inSize)) {
        IOFree(in, MLX_CMD_MAX_SIZE);
        return kIOReturnBadArgument;
    }
    MlxCmdInOut cmd = { in, inSize, out, sizeof(out), MLX_CMD_OP_CREATE_MKEY };
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_MKEY, in, inSize,
                                   out, sizeof(out), 5000);
    IOFree(in, MLX_CMD_MAX_SIZE);
    if (kr != kIOReturnSuccess)
        return kr;

    uint32_t mkeyIndex = static_cast<uint32_t>(mlxGetBits(out, 0x48, 24));
    uint32_t fullKey = mlxComposeMkey(mkeyIndex, variant);
    *mkey = fullKey;
    *lkey = fullKey;
    *rkey = fullKey;
    return kIOReturnSuccess;
}

kern_return_t MlxMR::cmdDestroyMKey(uint32_t mkey)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_MKEY);
    mlxSetBits(in, 0x48, 24, mlxMkeyIndex(mkey));
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
    if (!req->length || (req->accessFlags & ~MLX_MR_ACCESS_SUPPORTED) ||
        req->pd > 0xffffff || req->startAddr + req->length < req->startAddr)
        return kIOReturnBadArgument;

    /* Pin user memory (see ib_umem_get, mr.c) */
    IOMemoryDescriptor *mem = IOMemoryDescriptor::withAddressRange(
        req->startAddr, req->length,
        (req->accessFlags & (MLX_MR_ACCESS_LOCAL_WRITE |
                             MLX_MR_ACCESS_REMOTE_WRITE |
                             MLX_MR_ACCESS_REMOTE_ATOMIC)) ?
            kIODirectionInOut : kIODirectionIn,
        current_task());
    if (!mem)
        return kIOReturnNoMemory;
    /* Build the PBL */
    uint64_t *paList = static_cast<uint64_t *>(
        IOMallocZero(sizeof(uint64_t) * MLX_MAX_MR_SEGMENTS));
    if (!paList) {
        mem->release();
        return kIOReturnNoMemory;
    }
    uint32_t numSegs = 0;
    IODMACommand *dmaCommand = NULL;
    kern_return_t kr = buildPBL(mem, req->startAddr, req->length, paList,
                                &numSegs, &dmaCommand);
    if (kr != kIOReturnSuccess) {
        IOFree(paList, sizeof(uint64_t) * MLX_MAX_MR_SEGMENTS);
        mem->release();
        return kr;
    }

    /* Issue CREATE_MKEY */
    uint32_t mkey, lkey, rkey;
    kr = cmdCreateMKey(paList, numSegs, req->startAddr, req->length,
                       req->accessFlags, req->pd,
                       &mkey, &lkey, &rkey);
    IOFree(paList, sizeof(uint64_t) * MLX_MAX_MR_SEGMENTS);
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
            if (cmdDestroyMKey(mkey) == kIOReturnSuccess) {
                IOFree(ctx, sizeof(MlxMRContext));
                dmaCommand->clearMemoryDescriptor();
                dmaCommand->release();
                mem->release();
            } else {
                IOLog("MlxMR: quarantining MKey[%x] after record failure\n",
                      mkey);
            }
            return kIOReturnNoMemory;
        }
        IOLockLock(fLock);
        bool added = fMrTable->setObject(record);
        IOLockUnlock(fLock);
        record->release();
        if (!added) {
            if (cmdDestroyMKey(mkey) == kIOReturnSuccess) {
                IOFree(ctx, sizeof(MlxMRContext));
                dmaCommand->clearMemoryDescriptor();
                dmaCommand->release();
                mem->release();
            } else {
                IOLog("MlxMR: quarantining MKey[%x] after table failure\n",
                      mkey);
            }
            return kIOReturnNoMemory;
        }
    } else {
        if (cmdDestroyMKey(mkey) == kIOReturnSuccess) {
            dmaCommand->clearMemoryDescriptor();
            dmaCommand->release();
            mem->release();
        } else {
            IOLog("MlxMR: quarantining MKey[%x] after allocation failure\n",
                  mkey);
        }
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
