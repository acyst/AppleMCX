/*
 * MlxAH.cpp — Address Handle implementation (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/ah.c (create_ib_ah, ah.c:53)
 * Encodes the destination IP/GID/MAC into the hardware mlx5_av
 */
#include "MlxAH.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxAH, OSObject)

void MlxAH::free()
{
    if (fAhTable) {
        while (fAhTable->getCount()) {
            MlxAHContext *ctx = mlxRecordValue<MlxAHContext>(
                fAhTable->getObject(0));
            if (ctx)
                IOFree(ctx, sizeof(*ctx));
            fAhTable->removeObject(static_cast<unsigned int>(0));
        }
        fAhTable->release();
        fAhTable = NULL;
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    super::free();
}

bool MlxAH::init(MlxRoCE *roce)
{
    if (!super::init())
        return false;
    fRoce = roce;
    fAhTable = OSArray::withCapacity(16);
    fLock = IOLockAlloc();
    fNextHandle = 1;
    return fAhTable && fLock;
}

void MlxAH::encodeAV(const struct mlx_create_ah_req *req, MlxAV *av)
{
    /* Encode field by field (see create_ib_ah, ah.c:53-100) */
    memset(av, 0, sizeof(MlxAV));

    /* GRH segment (present in both modes: required for RoCE, optional for IB) */
    if (req->ahType == 0 || (req->dgid[0] || req->dgid[1])) {
        memcpy(av->rgid, req->dgid, 16);
        av->grh_gid_fl = OSSwapHostToBigInt32(
            MLX_AV_GRH_PRESENT | (req->sgidIndex << MLX_AV_SGID_INDEX_SHIFT));
        av->hop_limit = req->hopLimit ? req->hopLimit : 64;
        av->tclass = req->trafficClass;
    }

    av->stat_rate_sl = 0;

    if (req->ahType == 1) {
        /* IB mode addressing (see ah.c:96-99)
         * rlid: destination LID, fl_mlid: path bits, stat_rate_sl: SL (8 bits) */
        av->rlid = OSSwapHostToBigInt16(req->dlid);
        av->fl_mlid = req->pathBits & 0x7f;
        av->stat_rate_sl |= (req->sl & 0xf);
    } else {
        /* RoCE mode addressing (see ah.c:72-95) */
        av->hop_limit = req->hopLimit ? req->hopLimit : 64;
        av->tclass = req->trafficClass;
        av->tclass |= MLX_AV_ECN_ENABLED;   /* RoCEv2 ECN */
        memcpy(av->rmac, req->dmac, 6);
        av->udp_sport = OSSwapHostToBigInt16(req->udpSport ? req->udpSport
                                                           : MLX_ROCE_V2_UDP_DPORT);
        av->stat_rate_sl |= (0 & 0x7) << 1;   /* RoCE SL (low 3 bits) */
    }
}

kern_return_t MlxAH::createAH(const struct mlx_create_ah_req *req,
                              struct mlx_create_ah_resp *resp)
{
    if (!req || !resp)
        return kIOReturnBadArgument;

    MlxAHContext *ctx = (MlxAHContext *)IOMallocZero(sizeof(MlxAHContext));
    if (!ctx)
        return kIOReturnNoMemory;

    ctx->ahHandle = fNextHandle++;
    ctx->portNum = req->portNum ? req->portNum : 1;
    ctx->isRoCE = true;

    /* Encode the AV */
    encodeAV(req, &ctx->av);

    OSData *record = OSData::withBytesNoCopy(ctx, sizeof(*ctx));
    if (!record) {
        IOFree(ctx, sizeof(MlxAHContext));
        return kIOReturnNoMemory;
    }
    IOLockLock(fLock);
    bool added = fAhTable->setObject(record);
    IOLockUnlock(fLock);
    record->release();
    if (!added) {
        IOFree(ctx, sizeof(MlxAHContext));
        return kIOReturnNoMemory;
    }

    resp->ahHandle = ctx->ahHandle;

    IOLog("MlxAH: AH[%u] created dgid=%02x:%02x...:%02x sgid=%u dport=%u\n",
          ctx->ahHandle, req->dgid[0], req->dgid[1], req->dgid[15],
          req->sgidIndex, OSSwapHostToBigInt16(ctx->av.udp_sport));
    return kIOReturnSuccess;
}

kern_return_t MlxAH::destroyAH(uint32_t ahHandle)
{
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fAhTable->getCount(); i++) {
        MlxAHContext *ctx = mlxRecordValue<MlxAHContext>(
            fAhTable->getObject(i));
        if (ctx && ctx->ahHandle == ahHandle) {
            fAhTable->removeObject(i);
            IOFree(ctx, sizeof(MlxAHContext));
            IOLockUnlock(fLock);
            return kIOReturnSuccess;
        }
    }
    IOLockUnlock(fLock);
    return kIOReturnNotFound;
}

MlxAHContext *MlxAH::lookup(uint32_t ahHandle)
{
    MlxAHContext *found = NULL;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fAhTable->getCount(); i++) {
        MlxAHContext *ctx = mlxRecordValue<MlxAHContext>(
            fAhTable->getObject(i));
        if (ctx && ctx->ahHandle == ahHandle) {
            found = ctx;
            break;
        }
    }
    IOLockUnlock(fLock);
    return found;
}
