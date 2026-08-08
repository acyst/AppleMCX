/*
 * libmlx.c - usermode RDMA library implementation (macOS)
 *
 * Zero-copy data path core:
 *   post_send: writes WQE to SQ + rings UAR doorbell (64-bit atomic write)
 *   poll_cq:   reads the CQE buffer directly
 *
 * See wr.c:1051 (mlx5_ib_post_send) + cq.c:609 (mlx5_ib_poll_cq)
 */
#include "libmlx.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef __APPLE__
#include <IOKit/IOKitLib.h>
#endif

/* ---- Doorbell offsets (kept in sync with the kernel) ---- */
#define MLX_BF_OFFSET_USER      0x800
#define MLX_SND_DBR_USER        1

/* ========== Context structure ========== */

struct mlx_context {
    io_connect_t    conn;        /* IOUserClient connection */
    void           *uarMap;      /* usermode UAR mapping */
    uint32_t        deviceId;
    uint32_t        numPorts;
    char            devname[64]; /* device name (mlx5_N) */
};

struct mlx_pd {
    mlx_context *ctx;
    uint32_t    pd;
};

struct mlx_cq {
    mlx_context *ctx;
    uint32_t    cqHandle;
    struct MlxCqe64 *cqeBuf;   /* usermode CQ buffer */
    uint32_t    logSize;
    uint32_t    consIndex;
};

struct mlx_qp {
    mlx_context *ctx;
    uint32_t    qpn;
    uint32_t    qpType;
    void       *sqBuf;
    void       *rqBuf;
    uint32_t    sqSize;
    uint32_t    rqSize;
    void       *uar;
    uint32_t    bfOffset;
    uint32_t   *dbRecord;      /* send DB record */
    uint32_t    sqHead;        /* SQ software head */
    uint32_t    rqHead;
    uint32_t    state;
    uint64_t   *wrid;          /* wr_id corresponding to each completion */
    uint32_t    maxInline;
};

struct mlx_mr {
    mlx_context *ctx;
    uint32_t    mrHandle;
    uint32_t    lkey;
    uint32_t    rkey;
    void       *addr;
    uint64_t    length;
};

struct mlx_ah {
    mlx_context *ctx;
    uint32_t    ahHandle;
    struct MlxAV av;
};

/* ========== Utilities ========== */

static void
mem_barrier(void)
{
#ifdef __APPLE__
    __sync_synchronize();
#endif
}

static uint64_t
host_to_be64(uint64_t v)
{
    return __builtin_bswap64(v);
}

static uint32_t
host_to_be32(uint32_t v)
{
    return __builtin_bswap32(v);
}

static uint16_t
host_to_be16(uint16_t v)
{
    return __builtin_bswap16(v);
}

/* 64-bit atomic doorbell write (arm64 8-byte aligned volatile write) */
static void
write_doorbell(uint64_t val, void *addr)
{
    volatile uint64_t *db = (volatile uint64_t *)addr;
    *db = val;
}

/*
 * virt_to_phys - virtual address → physical address (for post_send data segments)
 * Queries pinned memory via the VirtToPhys method of MlxUserClient
 */
static uint64_t
virt_to_phys(mlx_context *ctx, uint64_t virt)
{
    uint64_t phys = 0;
    size_t outSize = 8;
    kern_return_t kr = IOConnectCallStructMethod(
        ctx->conn, kMlxUCMethodVirtToPhys, &virt, 8, &phys, &outSize);
    return (kr == kIOReturnSuccess) ? phys : 0;
}

/* ========== Device ========== */

mlx_context *mlx_open_device_by_name(const char *name)
{
    mlx_context *ctx = (mlx_context *)calloc(1, sizeof(mlx_context));
    if (!ctx)
        return NULL;

#ifdef __APPLE__
    /* Enumerate all MlxRoCE services, filter by the deviceName property */
    io_iterator_t iter = 0;
    io_service_t svc = 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, IOServiceMatching("MlxRoCE"), &iter);
    if (kr != kIOReturnSuccess) {
        free(ctx);
        return NULL;
    }

    io_service_t found = 0;
    while ((svc = IOIteratorNext(iter))) {
        if (!name || name[0] == '\0') {
            /* No name: take the first one */
            found = svc;
            break;
        }
        /* Read the deviceName property */
        CFTypeRef prop = IORegistryEntryCreateCFProperty(
            svc, CFSTR("deviceName"), kCFAllocatorDefault, 0);
        if (prop) {
            if (CFGetTypeID(prop) == CFStringGetTypeID()) {
                char buf[64] = {0};
                if (CFStringGetCString((CFStringRef)prop, buf, sizeof(buf),
                                       kCFStringEncodingUTF8) &&
                    strcmp(buf, name) == 0) {
                    found = svc;
                }
            }
            CFRelease(prop);
        }
        if (found)
            break;
        IOObjectRelease(svc);
    }
    if (!found) {
        IOObjectRelease(iter);
        free(ctx);
        return NULL;
    }
    kr = IOServiceOpen(found, mach_task_self(), 0, &ctx->conn);
    IOObjectRelease(found);
    IOObjectRelease(iter);
    if (kr != kIOReturnSuccess) {
        free(ctx);
        return NULL;
    }
    if (name)
        strncpy(ctx->devname, name, sizeof(ctx->devname) - 1);
#else
    (void)name;
#endif
    return ctx;
}

mlx_context *mlx_open_device(void)
{
    return mlx_open_device_by_name(NULL);
}

int mlx_list_devices(char **names, int max)
{
    if (!names || max <= 0)
        return 0;
    int count = 0;
#ifdef __APPLE__
    io_iterator_t iter = 0;
    io_service_t svc = 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, IOServiceMatching("MlxRoCE"), &iter);
    if (kr != kIOReturnSuccess)
        return 0;
    while ((svc = IOIteratorNext(iter)) && count < max) {
        CFTypeRef prop = IORegistryEntryCreateCFProperty(
            svc, CFSTR("deviceName"), kCFAllocatorDefault, 0);
        if (prop && CFGetTypeID(prop) == CFStringGetTypeID()) {
            char buf[64] = {0};
            if (CFStringGetCString((CFStringRef)prop, buf, sizeof(buf),
                                   kCFStringEncodingUTF8)) {
                names[count] = strdup(buf);
                if (names[count])
                    count++;
            }
            CFRelease(prop);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
#else
    (void)names; (void)max;
#endif
    return count;
}

void mlx_close_device(mlx_context *ctx)
{
    if (!ctx)
        return;
#ifdef __APPLE__
    if (ctx->conn)
        IOServiceClose(ctx->conn);
#endif
    free(ctx);
}

int mlx_query_device(mlx_context *ctx, struct mlx_query_device_resp *resp)
{
    if (!ctx || !resp)
        return EINVAL;
    size_t outSize = sizeof(struct mlx_query_device_resp);
    uint64_t inScalar[1] = {0};
    kern_return_t kr = IOConnectCallStructMethod(
        ctx->conn, kMlxUCMethodQueryDevice, inScalar, 0,
        resp, &outSize);
    return kr == kIOReturnSuccess ? 0 : EIO;
}

int mlx_query_port(mlx_context *ctx, struct mlx_query_port_resp *resp)
{
    if (!ctx || !resp)
        return EINVAL;
    size_t outSize = sizeof(struct mlx_query_port_resp);
    kern_return_t kr = IOConnectCallStructMethod(
        ctx->conn, kMlxUCMethodQueryPort, NULL, 0, resp, &outSize);
    return kr == kIOReturnSuccess ? 0 : EIO;
}

/* ========== PD ========== */

mlx_pd *mlx_alloc_pd(mlx_context *ctx)
{
    if (!ctx)
        return NULL;
    mlx_pd *pd = (mlx_pd *)calloc(1, sizeof(mlx_pd));
    if (!pd)
        return NULL;
    pd->ctx = ctx;
    pd->pd = 1;   /* MVP: PD 0 device default */
    return pd;
}

void mlx_dealloc_pd(mlx_pd *pd)
{
    free(pd);
}

/* ========== CQ ========== */

mlx_cq *mlx_create_cq(mlx_context *ctx, uint32_t cqe, uint32_t *cqHandle)
{
    if (!ctx || !cqHandle)
        return NULL;
    mlx_cq *cq = (mlx_cq *)calloc(1, sizeof(mlx_cq));
    if (!cq)
        return NULL;
    cq->ctx = ctx;
    cq->logSize = 0;
    while ((1u << cq->logSize) < cqe && cq->logSize < 16)
        cq->logSize++;
    cq->cqeBuf = NULL;

    /* Control path: create kernel CQ (with DMA CQE buffer) */
    uint32_t input = cqe;
    size_t outSize = 4;
    kern_return_t kr = IOConnectCallStructMethod(
        ctx->conn, kMlxUCMethodCreateCQ, &input, 4, cqHandle, &outSize);
    if (kr != kIOReturnSuccess) {
        free(cq);
        return NULL;
    }
    cq->cqHandle = *cqHandle;

    /* Zero-copy: map kernel CQE buffer (clientMemoryForType kMlxUCMemIndexCqe)
     * usermode polls hardware-written CQEs directly, no syscall */
#ifdef __APPLE__
    mach_vm_address_t mapped = 0;
    mach_vm_size_t mappedSize = 0;
    kr = IOConnectMapMemory(ctx->conn, kMlxUCMemIndexCqe, mach_task_self(),
                            &mapped, &mappedSize, kIOMapAnywhere);
    if (kr == kIOReturnSuccess && mapped) {
        cq->cqeBuf = (struct MlxCqe64 *)(uintptr_t)mapped;
    }
#endif
    if (!cq->cqeBuf) {
        /* Mapping failed fallback: local buffer */
        cq->cqeBuf = (struct MlxCqe64 *)calloc(1u << cq->logSize,
                                               sizeof(struct MlxCqe64));
    }
    return cq;
}

void mlx_destroy_cq(mlx_cq *cq)
{
    if (!cq)
        return;
    IOConnectCallStructMethod(cq->ctx->conn, kMlxUCMethodDestroyCQ,
                              &cq->cqHandle, 4, NULL, 0);
#ifdef __APPLE__
    if (cq->cqeBuf)
        IOConnectUnmapMemory(cq->ctx->conn, kMlxUCMemIndexCqe,
                             mach_task_self(), (mach_vm_address_t)(uintptr_t)cq->cqeBuf);
#endif
    free(cq);
}

/* ========== QP ========== */

mlx_qp *mlx_create_qp(mlx_context *ctx, mlx_pd *pd, mlx_cq *sendCq,
                      mlx_cq *recvCq, uint32_t qpType,
                      uint32_t sqSize, uint32_t rqSize,
                      void *sqBuf, void *rqBuf)
{
    if (!ctx || !sendCq)
        return NULL;
    mlx_qp *qp = (mlx_qp *)calloc(1, sizeof(mlx_qp));
    if (!qp)
        return NULL;
    qp->ctx = ctx;
    qp->qpType = qpType;
    qp->sqBuf = sqBuf;
    qp->rqBuf = rqBuf;
    qp->sqSize = sqSize;
    qp->rqSize = rqSize;
    qp->state = 0;   /* RST */
    qp->maxInline = 0;
    qp->wrid = (uint64_t *)calloc(sqSize, sizeof(uint64_t));
    if (!qp->wrid) {
        free(qp);
        return NULL;
    }

    struct mlx_create_qp_req req;
    memset(&req, 0, sizeof(req));
    req.pd = pd ? pd->pd : 1;
    req.sendCq = sendCq->cqHandle;
    req.recvCq = recvCq ? recvCq->cqHandle : sendCq->cqHandle;
    req.qpType = qpType;
    req.sqSize = sqSize;
    req.rqSize = rqSize;
    req.sqBufAddr = (uint64_t)(uintptr_t)sqBuf;
    req.rqBufAddr = (uint64_t)(uintptr_t)rqBuf;
    req.maxInlineData = 0;

    struct mlx_create_qp_resp resp;
    memset(&resp, 0, sizeof(resp));
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        ctx->conn, kMlxUCMethodCreateQP, &req, sizeof(req), &resp, &outSize);
    if (kr != kIOReturnSuccess) {
        free(qp->wrid);
        free(qp);
        return NULL;
    }
    qp->qpn = resp.qpn;

    /* Map DB record page (usermode post_send updates the queue head pointer)
     * See Linux uverbs mmap DB page */
#ifdef __APPLE__
    mach_vm_address_t dbAddr = 0;
    mach_vm_size_t dbSize = 0;
    kr = IOConnectMapMemory(ctx->conn, kMlxUCMemIndexDbRecord, mach_task_self(),
                            &dbAddr, &dbSize, kIOMapAnywhere);
    if (kr == kIOReturnSuccess && dbAddr)
        qp->dbRecord = (uint32_t *)(uintptr_t)dbAddr;
#endif
    return qp;
}

int mlx_modify_qp(mlx_qp *qp, uint32_t curState, uint32_t newState,
                  const struct mlx_modify_qp_req *attr)
{
    if (!qp)
        return EINVAL;
    struct mlx_modify_qp_req req;
    if (attr)
        memcpy(&req, attr, sizeof(req));
    else
        memset(&req, 0, sizeof(req));
    req.qpn = qp->qpn;
    req.curState = curState;
    req.newState = newState;
    kern_return_t kr = IOConnectCallStructMethod(
        qp->ctx->conn, kMlxUCMethodModifyQP, &req, sizeof(req), NULL, 0);
    if (kr != kIOReturnSuccess)
        return EIO;
    qp->state = newState;
    return 0;
}

void mlx_destroy_qp(mlx_qp *qp)
{
    if (!qp)
        return;
    IOConnectCallStructMethod(qp->ctx->conn, kMlxUCMethodDestroyQP,
                              &qp->qpn, 4, NULL, 0);
    free(qp->wrid);
    free(qp);
}

/* ========== MR ========== */

mlx_mr *mlx_reg_mr(mlx_pd *pd, void *addr, uint64_t length,
                   uint32_t accessFlags, struct mlx_reg_mr_resp *out)
{
    if (!pd || !addr || !out)
        return NULL;
    mlx_mr *mr = (mlx_mr *)calloc(1, sizeof(mlx_mr));
    if (!mr)
        return NULL;

    struct mlx_reg_mr_req req;
    memset(&req, 0, sizeof(req));
    req.startAddr = (uint64_t)(uintptr_t)addr;
    req.length = length;
    req.accessFlags = accessFlags;
    req.pd = pd->pd;

    struct mlx_reg_mr_resp resp;
    memset(&resp, 0, sizeof(resp));
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        pd->ctx->conn, kMlxUCMethodRegMR, &req, sizeof(req), &resp, &outSize);
    if (kr != kIOReturnSuccess) {
        free(mr);
        return NULL;
    }
    mr->ctx = pd->ctx;
    mr->mrHandle = resp.mrHandle;
    mr->lkey = resp.lkey;
    mr->rkey = resp.rkey;
    mr->addr = addr;
    mr->length = length;
    memcpy(out, &resp, sizeof(resp));
    return mr;
}

void mlx_dereg_mr(mlx_mr *mr)
{
    if (!mr)
        return;
    IOConnectCallStructMethod(mr->ctx->conn, kMlxUCMethodDeregMR,
                              &mr->mrHandle, 4, NULL, 0);
    free(mr);
}

/* ========== AH ========== */

mlx_ah *mlx_create_ah(mlx_context *ctx, const struct mlx_create_ah_req *req,
                      struct mlx_create_ah_resp *out)
{
    if (!ctx || !req || !out)
        return NULL;
    mlx_ah *ah = (mlx_ah *)calloc(1, sizeof(mlx_ah));
    if (!ah)
        return NULL;

    struct mlx_create_ah_resp resp;
    memset(&resp, 0, sizeof(resp));
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        ctx->conn, kMlxUCMethodCreateAH, req, sizeof(struct mlx_create_ah_req),
        &resp, &outSize);
    if (kr != kIOReturnSuccess) {
        free(ah);
        return NULL;
    }
    ah->ctx = ctx;
    ah->ahHandle = resp.ahHandle;
    memcpy(out, &resp, sizeof(resp));
    return ah;
}

void mlx_destroy_ah(mlx_ah *ah)
{
    if (!ah)
        return;
    IOConnectCallStructMethod(ah->ctx->conn, kMlxUCMethodDestroyAH,
                              &ah->ahHandle, 4, NULL, 0);
    free(ah);
}

/* ========== Data path (zero-copy core) ========== */

/*
 * post_send - writes WQE directly in usermode + doorbell
 * See wr.c:1051: WQE ctrl + data seg → DB record → 64-bit doorbell
 */
int mlx_post_send(mlx_qp *qp, const struct mlx_send_wr *wr)
{
    if (!qp || !wr)
        return EINVAL;

    /* Compute WQE size (ctrl 16B + data segments, 16B aligned) */
    uint32_t numDseg = (wr->length + 15) / 16;
    if (numDseg == 0)
        numDseg = 1;
    uint32_t wqeSize = (1 + numDseg) * 16;   /* ctrl + data segs */
    uint32_t wqeSize16 = wqeSize / 16;       /* in units of 16B */

    /* Get SQ slot */
    uint32_t idx = qp->sqHead & (qp->sqSize - 1);
    uint8_t *wqe = (uint8_t *)qp->sqBuf + (idx * 64);   /* 64B stride (MVP) */

    /* ctrl segment (See MlxWqeCtrlSeg) */
    struct MlxWqeCtrlSeg *ctrl = (struct MlxWqeCtrlSeg *)wqe;
    memset(ctrl, 0, 16);
    uint8_t opcode = (wr->wrType == MLX_WR_RDMA_WRITE) ? MLX_OPCODE_RDMA_WRITE :
                     (wr->wrType == MLX_WR_RDMA_READ) ? MLX_OPCODE_RDMA_READ :
                     MLX_OPCODE_SEND;
    /* opmod_idx_opcode: idx<<8 | opcode */
    ctrl->opmod_idx_opcode = host_to_be32((idx << 8) | opcode);
    /* qpn_ds: wqeSize16<<16 | qpn */
    ctrl->qpn_ds = host_to_be32((wqeSize16 << 16) | qp->qpn);
    ctrl->fm_ce_se = 0x02;   /* ce (generate completion event) */

    uint32_t off = 16;

    /* RDMA op: remote address segment (See MlxWqeRaddrSeg) */
    if (wr->wrType == MLX_WR_RDMA_WRITE || wr->wrType == MLX_WR_RDMA_READ) {
        struct MlxWqeRaddrSeg *raddr = (struct MlxWqeRaddrSeg *)(wqe + off);
        raddr->raddr = host_to_be64(wr->remoteAddr);
        raddr->rkey = host_to_be32(wr->rkey);
        off += 16;
    }

    /* Data segment (addr = physical address, for hardware DMA) */
    struct MlxWqeDataSeg *dseg = (struct MlxWqeDataSeg *)(wqe + off);
    dseg->byte_count = host_to_be32(wr->length);
    dseg->lkey = host_to_be32(wr->lkey);
    /* DMA attach: virtual → physical address (pinned) */
    uint64_t dataPhys = virt_to_phys(qp->ctx, (uint64_t)(uintptr_t)wr->data);
    dseg->addr = host_to_be64(dataPhys ? dataPhys
                                       : (uint64_t)(uintptr_t)wr->data);

    /* Record wr_id */
    qp->wrid[idx] = wr->wrId;

    mem_barrier();

    /* Update DB record */
    qp->sqHead++;
    if (qp->dbRecord)
        *qp->dbRecord = qp->sqHead;
    mem_barrier();

    /* Ring doorbell (64-bit atomic) */
    if (qp->uar) {
        uint64_t dbVal = ((uint64_t)(qp->sqHead & 0xFFFF) << 32) |
                         (uint64_t)qp->sqHead;
        write_doorbell(dbVal, (uint8_t *)qp->uar + MLX_BF_OFFSET_USER);
    }
    return 0;
}

/*
 * post_recv - prefills RQ SGE (See wr.c:1220)
 */
int mlx_post_recv(mlx_qp *qp, void *buf, uint32_t length, uint32_t lkey,
                  uint64_t wrId)
{
    if (!qp || !buf)
        return EINVAL;
    uint32_t idx = qp->rqHead & (qp->rqSize - 1);
    uint8_t *wqe = (uint8_t *)qp->rqBuf + (idx * 64);

    struct MlxWqeDataSeg *dseg = (struct MlxWqeDataSeg *)wqe;
    dseg->byte_count = host_to_be32(length);
    dseg->lkey = host_to_be32(lkey);
    /* DMA attach: receive buffer physical address */
    uint64_t bufPhys = virt_to_phys(qp->ctx, (uint64_t)(uintptr_t)buf);
    dseg->addr = host_to_be64(bufPhys ? bufPhys
                                      : (uint64_t)(uintptr_t)buf);

    qp->rqHead++;
    mem_barrier();
    if (qp->dbRecord)
        qp->dbRecord[1] = qp->rqHead;
    return 0;
}

/*
 * poll_cq - reads the CQE buffer directly (See cq.c:609)
 */
int mlx_poll_cq(mlx_cq *cq, struct MlxCqe64 *cqe, int num)
{
    if (!cq || !cqe || num <= 0)
        return EINVAL;
    uint32_t size = 1u << cq->logSize;
    int got = 0;
    for (int i = 0; i < num; i++) {
        struct MlxCqe64 *entry = &cq->cqeBuf[cq->consIndex & (size - 1)];
        /* Owner bit: (op_own >> 1) & 1 (See MLX_CQE_OWNER_MASK) */
        if (!(entry->op_own & MLX_CQE_OWNER_MASK))
            break;
        mem_barrier();
        memcpy(&cqe[i], entry, sizeof(struct MlxCqe64));
        /* Return ownership (clear the owner bit) */
        entry->op_own &= ~MLX_CQE_OWNER_MASK;
        cq->consIndex++;
        got++;
    }
    return got;
}

int mlx_query_cq_completions(mlx_context *ctx, uint32_t cqHandle,
                             uint64_t *count)
{
    if (!ctx || !count)
        return EINVAL;
    size_t outSize = 8;
    kern_return_t kr = IOConnectCallStructMethod(
        ctx->conn, kMlxUCMethodQueryCqCompletions, &cqHandle, 4,
        count, &outSize);
    return kr == kIOReturnSuccess ? 0 : EIO;
}

int mlx_get_async_event(mlx_context *ctx, struct mlx_async_event *event)
{
    if (!ctx || !event)
        return EINVAL;
    size_t outSize = sizeof(struct mlx_async_event);
    kern_return_t kr = IOConnectCallStructMethod(
        ctx->conn, kMlxUCMethodGetAsyncEvent, NULL, 0, event, &outSize);
    if (kr == kIOReturnNotFound)
        return EAGAIN;
    return kr == kIOReturnSuccess ? 0 : EIO;
}
