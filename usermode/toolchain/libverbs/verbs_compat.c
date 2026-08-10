/*
 * verbs_compat.c — RDMA verbs API compatibility implementation (macOS)
 *
 * Maps the Linux verbs API to libmlx (MlxUserClient).
 * Data path: reuses libmlx's zero-copy post_send/poll_cq.
 */
#include "verbs.h"
#include "libmlx.h"
#include "MlxUCIO.h"
#include "MlxWQE.hpp"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

/* ---- Device list (multi-device support) ---- */

#define MLX_MAX_DEVICES 8
static struct ibv_device *g_devices[MLX_MAX_DEVICES + 1] = { NULL };
static struct ibv_device g_device_arr[MLX_MAX_DEVICES];
static mlx_context *g_dev_ctx[MLX_MAX_DEVICES];   /* per-device libmlx ctx */
static struct ibv_cq *g_last_cq = NULL;   /* most recently created CQ (for cq_event) */

struct ibv_device **ibv_get_device_list(int *num_devices)
{
    if (num_devices)
        *num_devices = 0;
    if (!g_devices[0]) {
        /* Enumerate all MlxRoCE devices (by deviceName) */
        char *names[MLX_MAX_DEVICES] = {0};
        int n = mlx_list_devices(names, MLX_MAX_DEVICES);
        if (n <= 0) {
            /* fallback: try to open the default device */
            mlx_context *ctx = mlx_open_device();
            if (!ctx)
                return NULL;
            g_dev_ctx[0] = ctx;
            memset(&g_device_arr[0], 0, sizeof(g_device_arr[0]));
            strncpy(g_device_arr[0].name, "mlx5_0", IBV_SYSFS_NAME_MAX);
            strncpy(g_device_arr[0].dev_name, "mlx5_0", IBV_SYSFS_NAME_MAX);
            g_devices[0] = &g_device_arr[0];
            if (num_devices)
                *num_devices = 1;
            return g_devices;
        }
        for (int i = 0; i < n; i++) {
            mlx_context *ctx = mlx_open_device_by_name(names[i]);
            if (!ctx) {
                free(names[i]);
                continue;
            }
            g_dev_ctx[i] = ctx;
            memset(&g_device_arr[i], 0, sizeof(g_device_arr[i]));
            strncpy(g_device_arr[i].name, names[i], IBV_SYSFS_NAME_MAX);
            strncpy(g_device_arr[i].dev_name, names[i], IBV_SYSFS_NAME_MAX);
            g_devices[i] = &g_device_arr[i];
            free(names[i]);
        }
        g_devices[n] = NULL;   /* terminator */
        /* default active device = first one */
        if (num_devices)
            *num_devices = n;
    }
    return g_devices;
}

void ibv_free_device_list(struct ibv_device **list)
{
    (void)list;
}

const char *ibv_get_device_name(struct ibv_device *device)
{
    return device ? device->name : "mlx5_0";
}

/* ---- Context ---- */

struct ibv_context *ibv_open_device(struct ibv_device *device)
{
    if (!device)
        return NULL;
    struct ibv_context *ctx = (struct ibv_context *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->device = device;
    ctx->cmd_fd = 0;   /* MlxUserClient handle (managed via libmlx) */

    /* Bind this verbs context to its enumerated device. */
    for (int i = 0; i < MLX_MAX_DEVICES; i++) {
        if (g_devices[i] == device && g_dev_ctx[i]) {
            ctx->mlx_ctx = g_dev_ctx[i];
            break;
        }
    }
    if (!ctx->mlx_ctx) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

int ibv_close_device(struct ibv_context *context)
{
    if (context) {
        free(context);
    }
    return 0;
}

int ibv_query_device(struct ibv_context *context,
                     struct ibv_device_attr *device_attr)
{
    if (!context || !device_attr)
        return -1;
    struct mlx_query_device_resp resp;
    if (!context->mlx_ctx || mlx_query_device(context->mlx_ctx, &resp) != 0)
        return -1;
    memset(device_attr, 0, sizeof(*device_attr));
    device_attr->fw_ver = resp.fwVersion;
    device_attr->vendor_part_id = resp.deviceId;
    device_attr->max_qp = resp.maxQp;
    device_attr->max_cq = resp.maxCq;
    device_attr->max_mr = resp.maxMr;
    device_attr->max_qp_wr = 1u << 12;
    device_attr->max_cqe = 1u << 12;
    device_attr->max_sge = 4;
    device_attr->max_sge_rd = 4;
    device_attr->max_pd = 16;
    device_attr->max_ah = 128;
    device_attr->max_qp_rd_atom = 16;
    device_attr->max_qp_init_rd_atom = 16;
    device_attr->page_size_cap = 4096;
    device_attr->phys_port_cnt = resp.numPorts;
    device_attr->max_pkeys = 16;
    device_attr->num_comp_vectors = 4;
    device_attr->vendor_id = 0x15b3;
    return 0;
}

int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                   struct ibv_port_attr *port_attr)
{
    if (!context || !port_attr)
        return -1;
    struct mlx_query_port_resp resp;
    if (!context->mlx_ctx || mlx_query_port(context->mlx_ctx, &resp) != 0)
        return -1;
    memset(port_attr, 0, sizeof(*port_attr));
    port_attr->state = resp.portState ? IBV_PORT_ACTIVE : IBV_PORT_DOWN;
    port_attr->max_mtu = IBV_MTU_4096;
    port_attr->active_mtu = IBV_MTU_4096;
    port_attr->gid_tbl_len = resp.gidTblLen ? resp.gidTblLen : 128;
    port_attr->pkey_tbl_len = resp.pkeyTblLen ? resp.pkeyTblLen : 16;
    /* Link layer: 1=IB 2=Ethernet (see mlx_query_port_resp.linkLayer) */
    port_attr->link_layer = (resp.linkLayer == MLX_LINK_LAYER_INFINIBAND) ?
                            IBV_LINK_LAYER_INFINIBAND : IBV_LINK_LAYER_ETHERNET;
    port_attr->active_speed = 2;   /* 25Gb/s */
    /* IB attributes */
    port_attr->lid = resp.lid;
    port_attr->sm_lid = resp.smLid;
    return 0;
}

/* ---- PD / MR ---- */

struct ibv_pd *ibv_alloc_pd(struct ibv_context *context)
{
    if (!context)
        return NULL;
    struct ibv_pd *pd = (struct ibv_pd *)calloc(1, sizeof(*pd));
    if (!pd)
        return NULL;
    pd->context = context;
    pd->pd = 1;   /* MVP: device default PD */
    pd->mlx_pd = mlx_alloc_pd(context->mlx_ctx);
    if (!pd->mlx_pd) {
        free(pd);
        return NULL;
    }
    return pd;
}

int ibv_dealloc_pd(struct ibv_pd *pd)
{
    if (!pd)
        return EINVAL;
    mlx_dealloc_pd(pd->mlx_pd);
    free(pd);
    return 0;
}

struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
                          int access)
{
    if (!pd || !pd->context || !pd->context->mlx_ctx ||
        !pd->mlx_pd || !addr)
        return NULL;
    /* Convert access flags to libmlx */
    uint32_t flags = 0;
    if (access & IBV_ACCESS_LOCAL_WRITE) flags |= 0x2;
    if (access & IBV_ACCESS_REMOTE_WRITE) flags |= 0x8;
    if (access & IBV_ACCESS_REMOTE_READ) flags |= 0x4;
    struct mlx_reg_mr_resp resp;
    struct mlx_mr *mlxMr = mlx_reg_mr(pd->mlx_pd, addr, length, flags, &resp);
    if (!mlxMr)
        return NULL;
    struct ibv_mr *mr = (struct ibv_mr *)calloc(1, sizeof(*mr));
    if (mr) {
        mr->pd = pd;
        mr->addr = addr;
        mr->length = length;
        mr->lkey = resp.lkey;
        mr->rkey = resp.rkey;
        mr->mlx_mr = mlxMr;
    } else {
        mlx_dereg_mr(mlxMr);
    }
    return mr;
}

int ibv_dereg_mr(struct ibv_mr *mr)
{
    if (mr) {
        mlx_dereg_mr(mr->mlx_mr);
        free(mr);
    }
    return 0;
}

/* ---- CQ ---- */

struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe,
                             void *cq_context,
                             struct ibv_comp_channel *channel,
                             int comp_vector)
{
    if (!context || cqe <= 0)
        return NULL;
    struct ibv_cq *cq = (struct ibv_cq *)calloc(1, sizeof(*cq));
    if (!cq)
        return NULL;
    cq->context = context;
    cq->cq_context = cq_context;
    cq->channel = channel;
    cq->comp_vector = comp_vector;
    cq->cqe = cqe;

    /* Create the real libmlx CQ (via MlxUserClient, includes DMA CQE buffer) */
    if (context->mlx_ctx) {
        uint32_t cqHandle = 0;
        mlx_cq *mlxCq = mlx_create_cq(context->mlx_ctx, cqe, &cqHandle);
        if (mlxCq) {
            cq->handle = cqHandle;
            cq->mlx_cq = mlxCq;
            g_last_cq = cq;   /* recorded for ibv_get_cq_event */
            return cq;
        }
    }
    /* fallback: no libmlx association */
    free(cq);
    return NULL;
}

int ibv_destroy_cq(struct ibv_cq *cq)
{
    if (cq) {
        if (g_last_cq == cq)
            g_last_cq = NULL;
        if (cq->mlx_cq)
            mlx_destroy_cq(cq->mlx_cq);
        free(cq);
    }
    return 0;
}

/* ---- QP ---- */

struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                             struct ibv_qp_init_attr *qp_init_attr)
{
    if (!pd || !pd->context || !pd->context->mlx_ctx || !qp_init_attr)
        return NULL;
    struct ibv_qp *qp = (struct ibv_qp *)calloc(1, sizeof(*qp));
    if (!qp)
        return NULL;
    qp->context = pd->context;
    qp->qp_type = qp_init_attr->qp_type;
    qp->send_cq = qp_init_attr->send_cq;
    qp->recv_cq = qp_init_attr->recv_cq;
    qp->pd = pd;
    memcpy(&qp->cap, &qp_init_attr->cap, sizeof(qp->cap));

    /* Allocate SQ/RQ buffers and map them to the libmlx QP (control path) */
    uint32_t sqSize = qp_init_attr->cap.max_send_wr;
    uint32_t rqSize = qp_init_attr->cap.max_recv_wr;
    if (sqSize == 0) sqSize = 128;
    if (rqSize == 0) rqSize = 128;

    if (sqSize < 64) sqSize = 64;
    if (rqSize < 64) rqSize = 64;
    if (sqSize & (sqSize - 1)) {
        uint32_t value = 1;
        while (value < sqSize) value <<= 1;
        sqSize = value;
    }
    if (rqSize & (rqSize - 1)) {
        uint32_t value = 1;
        while (value < rqSize) value <<= 1;
        rqSize = value;
    }
    if (posix_memalign(&qp->sq_buf, 4096, (size_t)sqSize * 64) != 0 ||
        posix_memalign(&qp->rq_buf, 4096, (size_t)rqSize * 64) != 0) {
        free(qp->sq_buf);
        free(qp->rq_buf);
        free(qp);
        return NULL;
    }

    /* Create the real libmlx QP (via MlxUserClient)
     * CQ association: use send_cq's libmlx handle */
    mlx_cq *send_cq = qp_init_attr->send_cq ?
        ((struct ibv_cq *)qp_init_attr->send_cq)->mlx_cq : NULL;
    mlx_cq *recv_cq = qp_init_attr->recv_cq ?
        ((struct ibv_cq *)qp_init_attr->recv_cq)->mlx_cq : send_cq;
    qp->mlx_qp = mlx_create_qp(pd->context->mlx_ctx, pd->mlx_pd,
                               send_cq, recv_cq,
                               (qp_init_attr->qp_type == IBV_QPT_RC) ? 0 : 1,
                               sqSize, rqSize, qp->sq_buf, qp->rq_buf);
    if (!qp->mlx_qp) {
        free(qp->sq_buf);
        free(qp->rq_buf);
        free(qp);
        return NULL;
    }
    return qp;
}

int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                  int attr_mask)
{
    if (!qp || !attr)
        return -1;
    (void)attr_mask;
    int old_state = qp->state;

    /* State machine driven via libmlx (RST->INIT->RTR->RTS) */
    if (qp->mlx_qp) {
        struct mlx_modify_qp_req req;
        memset(&req, 0, sizeof(req));
        /* curState: inferred from the previous qp->state (MVP: sequential calls) */
        uint32_t cur = (old_state == IBV_QPS_RESET) ? 0 :
                       (old_state == IBV_QPS_INIT) ? 1 :
                       (old_state == IBV_QPS_RTR) ? 2 : 3;
        req.curState = cur;
        req.newState = attr->qp_state;
        req.destQpn = attr->dest_qp_num;
        req.pathMtu = attr->path_mtu;
        req.rqPsn = attr->rq_psn;
        req.sqPsn = attr->sq_psn;
        req.pkeyIndex = attr->pkey_index;
        req.portNum = attr->port_num;
        req.minRnrTimer = attr->min_rnr_timer;
        req.maxDestRdAtomic = attr->max_dest_rd_atomic;
        req.maxRdAtomic = attr->max_rd_atomic;
        /* AH (RTR encoding path): peer GID + source GID index */
        if (attr_mask & IBV_QP_AV) {
            memcpy(req.ahDgid, attr->ah_attr.grh.dgid.raw, 16);
            req.ahSgidIndex = attr->ah_attr.grh.sgid_index;
            req.ahHopLimit = attr->ah_attr.grh.hop_limit;
            req.ahTrafficClass = attr->ah_attr.grh.traffic_class;
            req.ahUdpSport = 0;   /* default assigned by kernel */
        }
        int rc = mlx_modify_qp(qp->mlx_qp, cur, attr->qp_state, &req);
        if (rc)
            return rc;
    }
    qp->state = attr->qp_state;
    return 0;
}

int ibv_destroy_qp(struct ibv_qp *qp)
{
    if (qp) {
        if (qp->mlx_qp)
            mlx_destroy_qp(qp->mlx_qp);
        free(qp->sq_buf);
        free(qp->rq_buf);
        free(qp);
    }
    return 0;
}

int ibv_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                 int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    (void)qp; (void)attr; (void)attr_mask; (void)init_attr;
    return -1;
}

/* ---- AH ---- */

struct ibv_ah *ibv_create_ah(struct ibv_pd *pd, struct ibv_ah_attr *attr)
{
    if (!pd || !pd->context || !pd->context->mlx_ctx || !attr)
        return NULL;
    struct ibv_ah *ah = (struct ibv_ah *)calloc(1, sizeof(*ah));
    if (ah) {
        ah->context = NULL;
        ah->pd = pd;
        memcpy(&ah->attr, attr, sizeof(*attr));
    }
    return ah;
}

int ibv_destroy_ah(struct ibv_ah *ah)
{
    if (ah)
        free(ah);
    return 0;
}

/* ---- Data path ---- */

int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                  struct ibv_send_wr **bad_wr)
{
    if (!qp) {
        if (bad_wr) *bad_wr = wr;
        return -1;
    }
    for (struct ibv_send_wr *w = wr; w; w = w->next) {
        struct mlx_send_wr mwr;
        memset(&mwr, 0, sizeof(mwr));
        switch (w->opcode) {
        case IBV_WR_RDMA_WRITE:
            mwr.wrType = MLX_WR_RDMA_WRITE;
            mwr.remoteAddr = w->wr.rdma.remote_addr;
            mwr.rkey = w->wr.rdma.rkey;
            break;
        case IBV_WR_RDMA_READ:
            mwr.wrType = MLX_WR_RDMA_READ;
            mwr.remoteAddr = w->wr.rdma.remote_addr;
            mwr.rkey = w->wr.rdma.rkey;
            break;
        case IBV_WR_SEND:
            mwr.wrType = MLX_WR_SEND;
            break;
        default:
            if (bad_wr) *bad_wr = w;
            return EOPNOTSUPP;
        }
        if (w->num_sge > 1) {
            if (bad_wr) *bad_wr = w;
            return EOPNOTSUPP;
        }
        if (w->sg_list && w->num_sge > 0) {
            mwr.data = (void *)(uintptr_t)w->sg_list[0].addr;
            mwr.length = w->sg_list[0].length;
            mwr.lkey = w->sg_list[0].lkey;
        }
        mwr.wrId = w->wr_id;

        /* libmlx data path (MVP: userland writes SQ directly + doorbell)
         * Full: requires a valid qp->mlx_qp, fallback recorded here */
        if (qp->mlx_qp) {
            int kr = mlx_post_send(qp->mlx_qp, &mwr);
            if (kr) {
                if (bad_wr) *bad_wr = w;
                return kr;
            }
        } else {
            if (bad_wr) *bad_wr = w;
            return ENODEV;
        }
    }
    return 0;
}

int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
                  struct ibv_recv_wr **bad_wr)
{
    if (!qp) {
        if (bad_wr) *bad_wr = wr;
        return -1;
    }
    for (struct ibv_recv_wr *w = wr; w; w = w->next) {
        if (w->num_sge != 1 || !w->sg_list || !qp->mlx_qp) {
            if (bad_wr) *bad_wr = w;
            return EOPNOTSUPP;
        }
        if (w->sg_list && w->num_sge > 0) {
            struct ibv_sge *sge = &w->sg_list[0];
            /* libmlx receive path (MVP) */
            if (qp->mlx_qp) {
                int kr = mlx_post_recv(qp->mlx_qp,
                              (void *)(uintptr_t)sge->addr,
                              sge->length, sge->lkey, w->wr_id);
                if (kr) {
                    if (bad_wr) *bad_wr = w;
                    return kr;
                }
            }
        }
    }
    return 0;
}

int ibv_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc)
{
    if (!cq || !wc || num_entries <= 0)
        return 0;
    /* Zero-copy CQE polling via libmlx (directly reads the mapped CQE buffer) */
    if (cq->mlx_cq) {
        struct MlxCqe64 cqes[8];
        int maxBatch = (num_entries > 8) ? 8 : num_entries;
        int got = mlx_poll_cq(cq->mlx_cq, cqes, maxBatch);
        for (int i = 0; i < got; i++) {
            memset(&wc[i], 0, sizeof(wc[i]));
            wc[i].wr_id = cqes[i].wqe_counter;   /* MVP: simplified wr_id */
            uint8_t opcode = cqes[i].op_own >> 4;
            wc[i].status = (opcode == 0x0d || opcode == 0x0e ||
                            opcode == 0x0c) ? IBV_WC_GENERAL_ERR :
                                               IBV_WC_SUCCESS;
            wc[i].byte_len = cqes[i].byte_cnt;
            wc[i].qp_num = cqes[i].sop_drop_qpn & 0xFFFFFF;
        }
        return got;
    }
    return 0;
}

int ibv_req_notify_cq(struct ibv_cq *cq, int solicited_only)
{
    (void)cq; (void)solicited_only;
    return 0;
}

/* ========== Supplementary API implementations (see rdma-core) ========== */

struct ibv_comp_channel *ibv_create_comp_channel(struct ibv_context *context)
{
    if (!context)
        return NULL;
    struct ibv_comp_channel *ch =
        (struct ibv_comp_channel *)calloc(1, sizeof(*ch));
    if (!ch)
        return NULL;
    ch->context = context;
    ch->fd = -1;   /* MVP: no event channel fd, poll mode */
    return ch;
}

int ibv_destroy_comp_channel(struct ibv_comp_channel *channel)
{
    if (channel)
        free(channel);
    return 0;
}

int ibv_query_gid(struct ibv_context *context, uint8_t port_num,
                  int index, union ibv_gid *gid)
{
    if (!context || !gid || port_num == 0 || index < 0)
        return -1;
    memset(gid, 0, sizeof(*gid));
    /* MVP: return the default IPv6 link-local GID (see mlx5_rdma_make_default_gid in rdma.c)
     * fe80:: + EUI-48 */
    gid->global.subnet_prefix = __builtin_bswap64(0xfe80000000000000ULL);
    gid->raw[8] = 0x02;   /* EUI-48 U/L bit */
    gid->raw[9] = 0x02;
    gid->raw[10] = 0xc9;
    gid->raw[11] = 0xff;
    gid->raw[12] = 0xfe;
    gid->raw[13] = 0x00;
    gid->raw[14] = 0x00;
    gid->raw[15] = 0x01;
    return 0;
}

int ibv_query_pkey(struct ibv_context *context, uint8_t port_num,
                   int index, uint16_t *pkey)
{
    if (!context || !pkey || port_num == 0 || index < 0)
        return -1;
    *pkey = 0xffff;   /* default pkey (full) */
    return 0;
}

int ibv_get_async_event(struct ibv_context *context,
                        struct ibv_async_event *event)
{
    if (!context || !event)
        return -1;
    /* Fetch async events via libmlx (non-blocking) */
    if (context->mlx_ctx) {
        struct mlx_async_event mev;
        if (mlx_get_async_event(context->mlx_ctx, &mev) == 0) {
            memset(event, 0, sizeof(*event));
            event->event_type = (enum ibv_event_type)mev.eventType;
            switch (mev.elementType) {
            case MLX_ASYNC_ELEMENT_CQ:   /* CQ */
                event->element.cq = NULL;
                break;
            case MLX_ASYNC_ELEMENT_QP:   /* QP */
                event->element.qp = NULL;
                break;
            case MLX_ASYNC_ELEMENT_PORT: /* port */
                event->element.port_num = (int)mev.elementHandle;
                break;
            default:   /* MLX_ASYNC_ELEMENT_DEVICE: device-level event, no element */
                break;
            }
            return 0;
        }
    }
    return -1;
}

int ibv_ack_async_event(struct ibv_async_event *event)
{
    (void)event;
    return 0;
}

int ibv_get_cq_event(struct ibv_comp_channel *channel,
                     struct ibv_cq **cq, void **cq_context)
{
    if (!channel || !cq)
        return -1;
    /* Query completion count via MlxUserClient (MVP: returns the most recently created CQ)
     * Full: blocks until a new completion, see ibv_get_cq_event semantics */
    if (g_last_cq && g_last_cq->context == channel->context &&
        channel->context->mlx_ctx) {
        uint64_t count = 0;
        if (mlx_query_cq_completions(channel->context->mlx_ctx,
                                     g_last_cq->handle, &count) != 0 ||
            count == g_last_cq->event_count)
            return -1;
        g_last_cq->event_count = count;
        *cq = g_last_cq;
        if (cq_context)
            *cq_context = (*cq)->cq_context;
        return 0;
    }
    return -1;
}

int ibv_ack_cq_events(struct ibv_cq *cq, unsigned int nevents)
{
    (void)cq; (void)nevents;
    return 0;
}
