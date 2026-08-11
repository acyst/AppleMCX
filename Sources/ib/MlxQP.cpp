/*
 * MlxQP.cpp — Queue Pair implementation (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/qp.c (create_user_qp, __mlx5_ib_modify_qp)
 *        + qpc.c (mlx5_qpc_create_qp)
 * Trimmed: RC/UD types only
 */
#include "MlxQP.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"
#include "MlxP0Encoding.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxQP, OSObject)

void MlxQP::free()
{
    if (fQpTable) {
        while (fQpTable->getCount()) {
            MlxQPContext *ctx = mlxRecordValue<MlxQPContext>(
                fQpTable->getObject(0));
            if (ctx) {
                uint32_t qpn = ctx->qpNum;
                if (fRoce && fRoce->getCore() &&
                    fRoce->getCore()->getCmd() &&
                    fRoce->getCore()->getCmd()->isUp()) {
                    if (destroyQP(qpn) == kIOReturnSuccess)
                        continue;
                }
                IOLog("MlxQP: quarantining QP[%u] DMA after unverified destroy\n",
                      qpn);
            }
            fQpTable->removeObject(static_cast<unsigned int>(0));
        }
        fQpTable->release();
        fQpTable = NULL;
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    super::free();
}

bool MlxQP::init(MlxRoCE *roce)
{
    if (!super::init())
        return false;
    fRoce = roce;
    fQpTable = OSArray::withCapacity(32);
    fLock = IOLockAlloc();
    return fQpTable && fLock;
}

kern_return_t MlxQP::createQP(const struct mlx_create_qp_req *req,
                              struct mlx_create_qp_resp *resp)
{
    /* See create_user_qp (qp.c:2294) + mlx5_qpc_create_qp (qpc.c:253)
     * create_qp_in = 64B header + opt_param_mask + ece + qpc(512B) + wq_umem */
    uint8_t in[MLX_CMD_MAX_SIZE] = {};
    uint8_t out[16] = {};
    uint32_t qpcOff = 0xc0 / 8;

    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_QP);
    mlxSetBits(in, 0x48, 24, 0);            /* input_qpn = allocate */
    mlxSetBits(in, 0x80, 32, 0);            /* opt_param_mask */

    /* QPC key fields (see mlx5_ifc.h:3355 qpc_bits) */
    uint8_t *qpc = in + qpcOff;
    /* st (8bit) at +0x00, pm_state at +0x10 */
    uint32_t st = (req->qpType == 0) ? MLX_QP_ST_RC : MLX_QP_ST_UD;
    if (!req->sqSize || !req->rqSize || req->sqSize < 64 ||
        req->rqSize < 64 ||
        (req->sqBufAddr & 0xfff) || (req->rqBufAddr & 0xfff) ||
        ((uint64_t)req->sqSize * 64) % 4096 ||
        ((uint64_t)req->rqSize * 64) % 4096 ||
        (req->sqSize & (req->sqSize - 1)) ||
        (req->rqSize & (req->rqSize - 1)))
        return kIOReturnBadArgument;
    uint32_t logSq = 31u - __builtin_clz(req->sqSize);
    uint32_t logRq = 31u - __builtin_clz(req->rqSize);
    if (logSq > 15 || logRq > 15)
        return kIOReturnBadArgument;

    mlxSetBits(qpc, 0x08, 8, st);
    mlxSetBits(qpc, 0x28, 24, req->pd);
    mlxSetBits(qpc, 0x49, 4, logRq);
    mlxSetBits(qpc, 0x4d, 3, 6); /* 64-byte RQ stride */
    mlxSetBits(qpc, 0x51, 4, logSq);
    mlxSetBits(qpc, 0x3e8, 24, req->sendCq);
    mlxSetBits(qpc, 0x4e8, 24, req->recvCq);
    uint32_t dbRecordOffset = 0;
    if (fRoce->getCore()->getUAR()->allocDbSlots(
            2, &dbRecordOffset) != kIOReturnSuccess)
        return kIOReturnNoResources;
    mlxSetBits(qpc, 0x500, 64,
               fRoce->getCore()->getUAR()->getDbRecordDMA() +
                   dbRecordOffset);

    /* WQ memory: wq_umem_valid at +0x800, wq_umem offset +0x810
     * DMA attach: pin the user SQ/RQ buffers, write the physical addresses into PAS */
    uint32_t pasOff = 0x880 / 8;
    uint64_t sqPhys = 0, rqPhys = 0;
    MlxDMAReq sqReq = {};
    MlxDMAReq rqReq = {};

    if (req->sqBufAddr && req->sqSize) {
        uint64_t sqLen = (uint64_t)req->sqSize * 64;   /* each WQE is 64B */
        if (fRoce->getCore()->getDMA()->pinUserMemory(
                req->sqBufAddr, sqLen, &sqReq) != kIOReturnSuccess)
        {
            fRoce->getCore()->getUAR()->freeDbSlots(dbRecordOffset, 2);
            return kIOReturnNoSpace;
        }
        sqPhys = sqReq.paList[0];
    }
    if (req->rqBufAddr && req->rqSize) {
        uint64_t rqLen = (uint64_t)req->rqSize * 64;
        if (fRoce->getCore()->getDMA()->pinUserMemory(
                req->rqBufAddr, rqLen, &rqReq) != kIOReturnSuccess) {
            if (sqReq.memDesc)
                fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
            fRoce->getCore()->getUAR()->freeDbSlots(dbRecordOffset, 2);
            return kIOReturnNoSpace;
        }
        rqPhys = rqReq.paList[0];
    }

    if (!sqReq.numSegs || !rqReq.numSegs ||
        sqReq.numSegs + rqReq.numSegs > MLX_DMA_MAX_SEGS) {
        if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
        if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
        fRoce->getCore()->getUAR()->freeDbSlots(dbRecordOffset, 2);
        return kIOReturnNoSpace;
    }

    mlxSetBits(in, 0x860, 1, 1);            /* wq_umem_valid */
    uint32_t pasIndex = 0;
    for (uint32_t i = 0; i < sqReq.numSegs; i++)
        OSWriteBigInt64(in, pasOff + 8 * pasIndex++, sqReq.paList[i]);
    for (uint32_t i = 0; i < rqReq.numSegs; i++)
        OSWriteBigInt64(in, pasOff + 8 * pasIndex++, rqReq.paList[i]);

    uint32_t inSize = pasOff + pasIndex * sizeof(uint64_t);
    MlxCmdInOut cmd = { in, inSize, out, sizeof(out),
                        MLX_CMD_OP_CREATE_QP };
    kern_return_t kr = fRoce->getCore()->exec(MLX_CMD_OP_CREATE_QP,
                                               in, inSize,
                                              out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        /* failure: unpin */
        if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
        if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
        fRoce->getCore()->getUAR()->freeDbSlots(dbRecordOffset, 2);
        return kr;
    }

    uint32_t qpn = static_cast<uint32_t>(mlxGetBits(out, 0x48, 24));
    resp->qpn = qpn;
    resp->sqStrideSize = 64;
    resp->dbRecordOffset = dbRecordOffset;
    resp->bfOffset = MLX_BF_OFFSET;

    /* Record the QP context */
    MlxQPContext *ctx = (MlxQPContext *)IOMallocZero(sizeof(MlxQPContext));
    if (!ctx) {
        if (destroyQP(qpn) == kIOReturnSuccess) {
            if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
            if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
            fRoce->getCore()->getUAR()->freeDbSlots(dbRecordOffset, 2);
        } else {
            IOLog("MlxQP: quarantining QP[%u] DMA after allocation failure\n",
                  qpn);
        }
        return kIOReturnNoMemory;
    }
    ctx->qpNum = qpn;
    ctx->state = MLX_QP_STATE_RST;
    ctx->st = st;
    ctx->pd = req->pd;
    ctx->sqSize = req->sqSize;
    ctx->rqSize = req->rqSize;
    ctx->sqBufAddr = req->sqBufAddr;
    ctx->rqBufAddr = req->rqBufAddr;
    ctx->dbRecordOffset = dbRecordOffset;
    ctx->bfOffset = MLX_BF_OFFSET;
    ctx->sqPhys = sqPhys;
    ctx->rqPhys = rqPhys;
    ctx->sqPinned = (sqReq.memDesc != NULL);
    ctx->rqPinned = (rqReq.memDesc != NULL);
    if (ctx->sqPinned)
        ctx->sqDma = sqReq;
    if (ctx->rqPinned)
        ctx->rqDma = rqReq;

    OSData *record = OSData::withBytesNoCopy(ctx, sizeof(*ctx));
    if (!record) {
        if (destroyQP(qpn) == kIOReturnSuccess) {
            IOFree(ctx, sizeof(MlxQPContext));
            if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
            if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
            fRoce->getCore()->getUAR()->freeDbSlots(dbRecordOffset, 2);
        } else {
            IOLog("MlxQP: quarantining QP[%u] after record allocation failure\n",
                  qpn);
        }
        return kIOReturnNoMemory;
    }
    IOLockLock(fLock);
    bool added = fQpTable->setObject(record);
    IOLockUnlock(fLock);
    record->release();
    if (!added) {
        if (destroyQP(qpn) == kIOReturnSuccess) {
            IOFree(ctx, sizeof(MlxQPContext));
            if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
            if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
            fRoce->getCore()->getUAR()->freeDbSlots(dbRecordOffset, 2);
        } else {
            IOLog("MlxQP: quarantining QP[%u] after table insertion failure\n",
                  qpn);
        }
        return kIOReturnNoMemory;
    }

    IOLog("MlxQP: QP[%u] created type=%s sq=%u rq=%u\n",
          qpn, (req->qpType == 0) ? "RC" : "UD", req->sqSize, req->rqSize);
    return kIOReturnSuccess;
}

kern_return_t MlxQP::modifyQP(const struct mlx_modify_qp_req *req)
{
    if (!req)
        return kIOReturnBadArgument;
    IOLockLock(fLock);
    MlxQPContext *ctx = NULL;
    for (uint32_t i = 0; i < fQpTable->getCount(); i++) {
        MlxQPContext *candidate = mlxRecordValue<MlxQPContext>(
            fQpTable->getObject(i));
        if (candidate && candidate->qpNum == req->qpn) {
            ctx = candidate;
            break;
        }
    }
    if (!ctx || ctx->state != req->curState) {
        IOLockUnlock(fLock);
        return ctx ? kIOReturnNotPermitted : kIOReturnNotFound;
    }
    /* See __mlx5_ib_modify_qp (qp.c:4166) + optab state machine
     * modify_qp_in layout (mlx5_ifc.h:5178):
     *   header + qpn@0x48 + opt_param_mask@0x80 + ece@0xa0 + qpc@0xc0
     * State transitions: RST→INIT (RST2INIT) / INIT→RTR (INIT2RTR) / RTR→RTS (RTR2RTS) */
    uint32_t opcode;
    switch (ctx->state) {
    case MLX_QP_STATE_RST:
        if (req->newState != MLX_QP_STATE_INIT) {
            IOLockUnlock(fLock);
            return kIOReturnNotPermitted;
        }
        opcode = MLX_CMD_OP_RST2INIT_QP;
        break;
    case MLX_QP_STATE_INIT:
        if (req->newState != MLX_QP_STATE_RTR) {
            IOLockUnlock(fLock);
            return kIOReturnNotPermitted;
        }
        opcode = MLX_CMD_OP_INIT2RTR_QP;
        break;
    case MLX_QP_STATE_RTR:
        if (req->newState != MLX_QP_STATE_RTS) {
            IOLockUnlock(fLock);
            return kIOReturnNotPermitted;
        }
        opcode = MLX_CMD_OP_RTR2RTS_QP;
        break;
    default:
        IOLockUnlock(fLock);
        return kIOReturnUnsupported;
    }

    uint8_t in[MLX_QP_MODIFY_IN_BYTES] = {};
    uint8_t out[16] = {};
    uint32_t qpcOff = MLX_QPC_BIT_OFFSET / 8;

    OSWriteBigInt16(in, 0, opcode);
    mlxSetBits(in, 0x48, 24, req->qpn);
    uint32_t optParamMask = 0;

    /* QPC */
    uint8_t *qpc = in + qpcOff;

    /* pkey_index is in the primary ADS, not at next_send_psn@0x3c8. */
    if (opcode == MLX_CMD_OP_RST2INIT_QP) {
        if (!mlxEncodeRst2InitQpc(qpc, MLX_QPC_BYTES, req->pkeyIndex,
                                  req->portNum, &optParamMask)) {
            IOLockUnlock(fLock);
            return kIOReturnBadArgument;
        }
        ctx->pkeyIndex = static_cast<uint16_t>(req->pkeyIndex);
        ctx->portNum = static_cast<uint8_t>(req->portNum);
    }
    if (opcode == MLX_CMD_OP_INIT2RTR_QP) {
        uint32_t dqpn = req->destQpn;
        /* Fill the RoCE path data into ctx (see mlx5_set_path, qp.c:3583)
         * AH fields: destination MAC/GID, source GID index, TTL, DSCP, UDP sport */
        memcpy(ctx->ahDmac, req->ahDmac, 6);
        memcpy(ctx->ahDgid, req->ahDgid, 16);
        ctx->ahSgidIndex = req->ahSgidIndex;
        ctx->ahHopLimit = req->ahHopLimit;
        ctx->ahTrafficClass = req->ahTrafficClass;
        ctx->ahUdpSport = req->ahUdpSport;
        ctx->destQpn = dqpn;
        struct MlxRocePathFields path = {};
        memcpy(path.dmac, ctx->ahDmac, sizeof(path.dmac));
        memcpy(path.dgid, ctx->ahDgid, sizeof(path.dgid));
        path.sgidIndex = ctx->ahSgidIndex;
        path.hopLimit = ctx->ahHopLimit;
        path.trafficClass = ctx->ahTrafficClass;
        path.udpSport = ctx->ahUdpSport;
        path.pkeyIndex = ctx->pkeyIndex;
        path.portNum = ctx->portNum;
        if (!mlxEncodeInit2RtrQpc(qpc, MLX_QPC_BYTES, &path, dqpn,
                                  req->pathMtu, req->rqPsn,
                                  req->maxDestRdAtomic, &optParamMask)) {
            IOLockUnlock(fLock);
            return kIOReturnBadArgument;
        }
    }
    if (opcode == MLX_CMD_OP_RTR2RTS_QP) {
        if (!mlxEncodeRtr2RtsQpc(qpc, MLX_QPC_BYTES, req->sqPsn,
                                 req->minRnrTimer, req->maxRdAtomic,
                                 &optParamMask)) {
            IOLockUnlock(fLock);
            return kIOReturnBadArgument;
        }
    }

    mlxSetBits(in, 0x80, 32, optParamMask);

    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out), opcode };
    kern_return_t kr = fRoce->getCore()->exec(opcode, in, sizeof(in),
                                              out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess)
        ctx->state = req->newState;
    IOLockUnlock(fLock);
    return kr;
}

MlxQPContext *MlxQP::ctxForQpn(uint32_t qpn)
{
    return lookup(qpn);
}

kern_return_t MlxQP::destroyQP(uint32_t qpn)
{
    /* See qpc.c mlx5_qpc_destroy_qp */
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_QP);
    mlxSetBits(in, 0x48, 24, qpn);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_DESTROY_QP };
    kern_return_t kr = fRoce->getCore()->exec(MLX_CMD_OP_DESTROY_QP,
                                              in, sizeof(in),
                                              out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) {
        IOLockLock(fLock);
        for (uint32_t i = 0; i < fQpTable->getCount(); i++) {
            MlxQPContext *ctx = mlxRecordValue<MlxQPContext>(
                fQpTable->getObject(i));
            if (ctx && ctx->qpNum == qpn) {
                /* Release the DMA pins */
                if (ctx->sqPinned)
                    fRoce->getCore()->getDMA()->unpinMemory(&ctx->sqDma);
                if (ctx->rqPinned)
                    fRoce->getCore()->getDMA()->unpinMemory(&ctx->rqDma);
                fRoce->getCore()->getUAR()->freeDbSlots(
                    ctx->dbRecordOffset, 2);
                fQpTable->removeObject(i);
                IOFree(ctx, sizeof(MlxQPContext));
                break;
            }
        }
        IOLockUnlock(fLock);
        IOLog("MlxQP: QP[%u] destroyed\n", qpn);
    }
    return kr;
}

kern_return_t MlxQP::queryQP(uint32_t qpn, void *out)
{
    (void)qpn;
    (void)out;
    return kIOReturnUnsupported;
}

void MlxQP::handleQPEvent(uint32_t qpn, uint32_t event)
{
    (void)qpn;
    (void)event;
}

MlxQPContext *MlxQP::lookup(uint32_t qpn)
{
    MlxQPContext *found = NULL;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fQpTable->getCount(); i++) {
        MlxQPContext *ctx = mlxRecordValue<MlxQPContext>(
            fQpTable->getObject(i));
        if (ctx && ctx->qpNum == qpn) {
            found = ctx;
            break;
        }
    }
    IOLockUnlock(fLock);
    return found;
}
