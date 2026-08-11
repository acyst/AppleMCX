/*
 * MlxQP.hpp — Queue Pair management (generic Mellanox family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/infiniband/hw/mlx5/qp.c
 * Trimmed: RC/UD types only, supporting RDMA WRITE/READ/SEND + UD datagram
 */
#ifndef MLX_QP_HPP
#define MLX_QP_HPP

#include <libkern/OSTypes.h>
#include <IOKit/IOLocks.h>
#include "MlxRegs.hpp"
#include "MlxWQE.hpp"
#include "MlxDMA.hpp"
#include "MlxUCIO.h"

class MlxRoCE;
class MlxCQ;

/*
 * QP context (see struct mlx5_ib_qp + QPC)
 */
struct MlxQPContext {
    uint32_t    qpNum;
    uint32_t    state;          /* see MLX_QP_STATE_* */
    uint32_t    st;             /* MLX_QP_ST_RC / MLX_QP_ST_UD */
    uint32_t    pd;
    MlxCQ      *sendCq;
    MlxCQ      *recvCq;
    /* WQ */
    uint64_t    sqBufAddr;      /* user SQ buffer */
    uint64_t    rqBufAddr;      /* user RQ buffer */
    uint32_t    sqSize;
    uint32_t    rqSize;
    uint32_t    curPost;        /* SQ software head pointer */
    uint32_t    curRecv;        /* RQ software head pointer */
    uint32_t    dbRecordOffset; /* DB record user offset */
    uint32_t    bfOffset;       /* BF doorbell user offset */
    uint64_t    sqPhys;         /* SQ physical address (DMA) */
    uint64_t    rqPhys;         /* RQ physical address (DMA) */
    MlxDMAReq   sqDma;          /* SQ pin record */
    MlxDMAReq   rqDma;          /* RQ pin record */
    bool        sqPinned;
    bool        rqPinned;
    /* path (RC) */
    uint8_t     ahDmac[6];
    uint8_t     ahDgid[16];
    uint32_t    ahSgidIndex;
    uint8_t     ahHopLimit;
    uint8_t     ahTrafficClass;
    uint16_t    ahUdpSport;
    uint16_t    pkeyIndex;
    uint8_t     portNum;
    uint32_t    destQpn;
    uint32_t    rqPsn;
    uint32_t    sqPsn;
    /* counters */
    uint64_t    sqPkts;
    uint64_t    rqPkts;
};

/*
 * QP management class
 */
class MlxQP : public OSObject {
    OSDeclareDefaultStructors(MlxQP)

public:
    bool init(MlxRoCE *roce);
    virtual void free() APPLE_KEXT_OVERRIDE;

    kern_return_t createQP(const struct mlx_create_qp_req *req,
                           struct mlx_create_qp_resp *resp);
    kern_return_t modifyQP(const struct mlx_modify_qp_req *req);
    kern_return_t destroyQP(uint32_t qpn);
    kern_return_t queryQP(uint32_t qpn, void *out);

    /* Event handling: report state changes (see mlx5_ib_qp_event, qp.c:423) */
    void handleQPEvent(uint32_t qpn, uint32_t event);

    /* For poll_cq lookups */
    MlxQPContext *lookup(uint32_t qpn);

private:
    /* QPC construction (see create_user_qp, qp.c:2294) */
    kern_return_t buildQPC(MlxQPContext *qp, struct MlxQPCmdBuf *out);

    /* State transition (see __mlx5_ib_modify_qp + optab, qp.c:4166) */
    kern_return_t stateTransition(MlxQPContext *qp,
                                  uint32_t cur, uint32_t newState,
                                  const struct mlx_modify_qp_req *req);

    /* Helper: find by qpn (internal) */
    MlxQPContext *ctxForQpn(uint32_t qpn);

    MlxRoCE     *fRoce;
    OSArray     *fQpTable;
    IOLock      *fLock;
};

#endif /* MLX_QP_HPP */
