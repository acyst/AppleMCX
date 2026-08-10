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

#include <string.h>
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxQP, OSObject)

bool MlxQP::init(MlxRoCE *roce)
{
    if (!super::init())
        return false;
    fRoce = roce;
    fQpTable = OSArray::withCapacity(32);
    fLock = IOLockAlloc();
    return true;
}

kern_return_t MlxQP::createQP(const struct mlx_create_qp_req *req,
                              struct mlx_create_qp_resp *resp)
{
    /* See create_user_qp (qp.c:2294) + mlx5_qpc_create_qp (qpc.c:253)
     * create_qp_in = 64B header + opt_param_mask + ece + qpc(512B) + wq_umem */
    uint8_t in[4096] = {};
    uint8_t out[64] = {};
    uint32_t qpcOff = 0x40 + 0x20 + 0x20;   /* header 64 + opt_param(32) + ece(32) */

    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_QP);
    OSWriteBigInt16(in, 2, 0);              /* uid (only used for resource 0) */
    OSWriteBigInt32(in, 0x30, 0);           /* input_qpn = 0 (allocate) */
    OSWriteBigInt32(in, 0x40, 0);           /* opt_param_mask */

    /* QPC key fields (see mlx5_ifc.h:3355 qpc_bits) */
    uint8_t *qpc = in + qpcOff;
    /* st (8bit) at +0x00, pm_state at +0x10 */
    uint32_t st = (req->qpType == 0) ? MLX_QP_ST_RC : MLX_QP_ST_UD;
    qpc[0x00] = (uint8_t)(st & 0xFF);
    qpc[0x10] = 0;                          /* pm_state = 0 (use PM_NONE) */
    /* pd (24bit) at +0x30 */
    qpc[0x30] = (uint8_t)((req->pd >> 16) & 0xFF);
    qpc[0x31] = (uint8_t)((req->pd >> 8) & 0xFF);
    qpc[0x32] = (uint8_t)(req->pd & 0xFF);
    /* log_sq_size (4bit) at +0x54, log_rq_size (4bit) at +0x48 */
    qpc[0x54] = (uint8_t)((req->sqSize & 0xF) << 4);
    qpc[0x48] = (uint8_t)((req->rqSize & 0xF) << 4);
    /* cqn_snd (24bit) at +0x3E8 */
    uint32_t cqnSnd = req->sendCq;
    qpc[0x3E8] = (uint8_t)((cqnSnd >> 16) & 0xFF);
    qpc[0x3E9] = (uint8_t)((cqnSnd >> 8) & 0xFF);
    qpc[0x3EA] = (uint8_t)(cqnSnd & 0xFF);
    /* cqn_rcv (24bit) at +0x4E8 */
    uint32_t cqnRcv = req->recvCq;
    qpc[0x4E8] = (uint8_t)((cqnRcv >> 16) & 0xFF);
    qpc[0x4E9] = (uint8_t)((cqnRcv >> 8) & 0xFF);
    qpc[0x4EA] = (uint8_t)(cqnRcv & 0xFF);
    /* dbr_addr (64bit) at +0x4F0 */
    OSWriteBigInt64(qpc, 0x4F0, req->dbRecordOffset);
    /* q_key (32bit) at +0x4F8 */
    OSWriteBigInt32(qpc, 0x4F8, 0);

    /* WQ memory: wq_umem_valid at +0x800, wq_umem offset +0x810
     * DMA attach: pin the user SQ/RQ buffers, write the physical addresses into PAS */
    uint32_t wqOff = 0x800;
    uint64_t sqPhys = 0, rqPhys = 0;
    MlxDMAReq sqReq = {};
    MlxDMAReq rqReq = {};

    if (req->sqBufAddr && req->sqSize) {
        uint64_t sqLen = (uint64_t)req->sqSize * 64;   /* each WQE is 64B */
        if (fRoce->getCore()->getDMA()->pinUserMemory(
                req->sqBufAddr, sqLen, &sqReq) == kIOReturnSuccess) {
            sqPhys = sqReq.paList[0];
        }
    }
    if (req->rqBufAddr && req->rqSize) {
        uint64_t rqLen = (uint64_t)req->rqSize * 64;
        if (fRoce->getCore()->getDMA()->pinUserMemory(
                req->rqBufAddr, rqLen, &rqReq) == kIOReturnSuccess) {
            rqPhys = rqReq.paList[0];
        }
    }

    in[wqOff] |= 0x80;                      /* wq_umem_valid bit0 at +0x800 bit7 */
    /* wq_umem PAS (physical addresses) */
    OSWriteBigInt64(in, wqOff + 0x10, sqPhys);
    OSWriteBigInt64(in, wqOff + 0x18, rqPhys);

    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_CREATE_QP };
    kern_return_t kr = fRoce->getCore()->exec(MLX_CMD_OP_CREATE_QP,
                                              in, sizeof(in),
                                              out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        /* failure: unpin */
        if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
        if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
        return kr;
    }

    /* create_qp_out: qpn at +0x40+8 (24bit) */
    uint32_t qpn = OSReadBigInt32(out, 0x48) & 0xFFFFFF;
    resp->qpn = qpn;
    resp->sqStrideSize = 64;
    resp->sqPhys = sqPhys;
    resp->rqPhys = rqPhys;

    /* Record the QP context */
    MlxQPContext *ctx = (MlxQPContext *)IOMallocZero(sizeof(MlxQPContext));
    if (!ctx) {
        destroyQP(qpn);
        if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
        if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
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
    ctx->dbRecordOffset = req->dbRecordOffset;
    ctx->bfOffset = req->bfOffset;
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
        IOFree(ctx, sizeof(MlxQPContext));
        destroyQP(qpn);
        if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
        if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
        return kIOReturnNoMemory;
    }
    IOLockLock(fLock);
    bool added = fQpTable->setObject(record);
    IOLockUnlock(fLock);
    record->release();
    if (!added) {
        IOFree(ctx, sizeof(MlxQPContext));
        destroyQP(qpn);
        if (sqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&sqReq);
        if (rqReq.memDesc) fRoce->getCore()->getDMA()->unpinMemory(&rqReq);
        return kIOReturnNoMemory;
    }

    IOLog("MlxQP: QP[%u] created type=%s sq=%u rq=%u\n",
          qpn, (req->qpType == 0) ? "RC" : "UD", req->sqSize, req->rqSize);
    return kIOReturnSuccess;
}

kern_return_t MlxQP::modifyQP(const struct mlx_modify_qp_req *req)
{
    /* See __mlx5_ib_modify_qp (qp.c:4166) + optab state machine
     * modify_qp_in layout (mlx5_ifc.h:5178):
     *   header 64B + qpn@0x40 + opt_param_mask@0x60 + ece@0x80 + qpc@0xA0
     * State transitions: RST→INIT (RST2INIT) / INIT→RTR (INIT2RTR) / RTR→RTS (RTR2RTS) */
    uint32_t opcode;
    switch (req->curState) {
    case MLX_QP_STATE_RST:
        opcode = MLX_CMD_OP_RST2INIT_QP;
        break;
    case MLX_QP_STATE_INIT:
        opcode = MLX_CMD_OP_INIT2RTR_QP;
        break;
    case MLX_QP_STATE_RTR:
        opcode = MLX_CMD_OP_RTR2RTS_QP;
        break;
    default:
        return kIOReturnUnsupported;
    }

    uint8_t in[1024] = {};
    uint8_t out[32] = {};
    uint32_t qpcOff = 0xA0;     /* modify_qp_in: header 64 + opt_param@0x60 + ece@0x80 */

    OSWriteBigInt16(in, 0, opcode);
    OSWriteBigInt32(in, 0x40, req->qpn);
    OSWriteBigInt32(in, 0x60, req->attrMask);   /* opt_param_mask */

    /* QPC */
    uint8_t *qpc = in + qpcOff;
    qpc[0x00] = MLX_QP_ST_RC;   /* st (MVP: RC) */
    /* pm_state at +0x10 bits[3:2] */
    qpc[0x10] = (uint8_t)((req->curState & 0x3) << 2) | (req->newState & 0x3);

    /* pkey_index + destination QPN (see qpc_bits: pkey_index@0x3C8, remote_qpn@0xA8) */
    if (opcode == MLX_CMD_OP_RST2INIT_QP) {
        /* pkey_index at qpc +0x3C8 (low 16 bits) */
        OSWriteBigInt16(qpc, 0x3C8, req->pkeyIndex);
    }
    if (opcode == MLX_CMD_OP_INIT2RTR_QP) {
        uint32_t dqpn = req->destQpn;
        qpc[0xA8] = (uint8_t)((dqpn >> 16) & 0xFF);
        qpc[0xA9] = (uint8_t)((dqpn >> 8) & 0xFF);
        qpc[0xAA] = (uint8_t)(dqpn & 0xFF);
        /* mtu@0x40 (3bit), path_mtu = req->pathMtu */
        qpc[0x40] = (uint8_t)((qpc[0x40] & 0xE0) | (req->pathMtu & 0x7));
        /* rq_psn: next_rcv_psn@0x4A8 */
        OSWriteBigInt32(qpc, 0x4A8, req->rqPsn);
        /* Fill the RoCE path data into ctx (see mlx5_set_path, qp.c:3583)
         * AH fields: destination MAC/GID, source GID index, TTL, DSCP, UDP sport */
        MlxQPContext *ctx = ctxForQpn(req->qpn);
        if (ctx) {
            memcpy(ctx->ahDmac, req->ahDmac, 6);
            memcpy(ctx->ahDgid, req->ahDgid, 16);
            ctx->ahSgidIndex = req->ahSgidIndex;
            ctx->ahHopLimit = req->ahHopLimit;
            ctx->ahTrafficClass = req->ahTrafficClass;
            ctx->ahUdpSport = req->ahUdpSport;
            ctx->destQpn = dqpn;
        }
        /* Encode the RoCE path into primary_address_path (ads@0xC0) */
        encodePath(ctx, qpc + 0xC0);
    }
    if (opcode == MLX_CMD_OP_RTR2RTS_QP) {
        /* sq_psn: next_send_psn@0x3C8 (note it is adjacent to pkey_index) */
        OSWriteBigInt32(qpc, 0x3C8, req->sqPsn);
        /* min_rnr_timer: min_rnr_nak@0x4A0 (5bit) */
        qpc[0x4A0] = (uint8_t)((qpc[0x4A0] & 0xE0) | (req->minRnrTimer & 0x1F));
    }

    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out), opcode };
    kern_return_t kr = fRoce->getCore()->exec(opcode, in, sizeof(in),
                                              out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) {
        MlxQPContext *ctx = lookup(req->qpn);
        if (ctx)
            ctx->state = req->newState;
    }
    return kr;
}

/* Encode the RoCE path into ads (see mlx5_set_path, qp.c:3583)
 * ads layout (mlx5_ifc.h:780):
 *   src_addr_index@0x00, hop_limit@0x08, tclass@0x0C, flow_label@0x10,
 *   rgid_rip@0x14, dscp@0x78(6bit), udp_sport@0x7C, eth_prio@0x80 */
void MlxQP::encodePath(MlxQPContext *qp, void *ads)
{
    if (!qp || !ads)
        return;
    uint8_t *a = (uint8_t *)ads;
    /* src_addr_index (source GID table index) */
    a[0x00] = (uint8_t)(qp->ahSgidIndex & 0xFF);
    /* hop_limit (TTL) */
    a[0x08] = qp->ahHopLimit ? qp->ahHopLimit : 64;
    /* tclass (DSCP + ECN) */
    a[0x0C] = qp->ahTrafficClass | MLX_AV_ECN_ENABLED;
    /* destination GID (remote IP) */
    memcpy(a + 0x14, qp->ahDgid, 16);
    /* destination MAC (rmac_47_32 in the ads +0x70 area) */
    memcpy(a + 0x70, qp->ahDmac, 6);
    /* dscp@0x78 (6bit): tclass>>2 */
    a[0x78] = (uint8_t)((qp->ahTrafficClass >> 2) & 0x3F);
    /* udp_sport@0x7C */
    OSWriteBigInt16(a, 0x7C, qp->ahUdpSport);
    /* eth_prio@0x80 (3bit) */
    a[0x80] = (uint8_t)((a[0x80] & 0xF8) | 0);
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
    OSWriteBigInt32(in, 4, qpn);
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
