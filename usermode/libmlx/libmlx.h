/*
 * libmlx.h - usermode RDMA library interface (macOS)
 *
 * Zero-copy data path:
 *   - post_send: usermode writes WQE directly to the SQ buffer + rings UAR doorbell
 *   - poll_cq:   usermode reads the CQE buffer directly
 *   - Control path: IOConnectCallMethod (create/modify/destroy)
 */
#ifndef LIBMLX_H
#define LIBMLX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "MlxUCIO.h"
#include "MlxWQE.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/* Device handles */
typedef struct mlx_context mlx_context;
typedef struct mlx_pd mlx_pd;
typedef struct mlx_qp mlx_qp;
typedef struct mlx_cq mlx_cq;
typedef struct mlx_mr mlx_mr;
typedef struct mlx_ah mlx_ah;

/* ========== Device ========== */
mlx_context *mlx_open_device(void);
void mlx_close_device(mlx_context *ctx);

/* Multiple devices: enumerate available devices (returns an array of device names; caller frees) */
int mlx_list_devices(char **names, int max);
/* Open by device name (e.g. "mlx5_0"); empty name opens the first */
mlx_context *mlx_open_device_by_name(const char *name);
int mlx_query_device(mlx_context *ctx, struct mlx_query_device_resp *resp);
int mlx_query_port(mlx_context *ctx, struct mlx_query_port_resp *resp);

/* ========== PD ========== */
mlx_pd *mlx_alloc_pd(mlx_context *ctx);
void mlx_dealloc_pd(mlx_pd *pd);

/* ========== CQ ========== */
mlx_cq *mlx_create_cq(mlx_context *ctx, uint32_t cqe, uint32_t *cqHandle);
void mlx_destroy_cq(mlx_cq *cq);

/* ========== QP ========== */
mlx_qp *mlx_create_qp(mlx_context *ctx, mlx_pd *pd,
                      mlx_cq *sendCq, mlx_cq *recvCq,
                      uint32_t qpType, uint32_t sqSize, uint32_t rqSize,
                      void *sqBuf, void *rqBuf);
int mlx_modify_qp(mlx_qp *qp, uint32_t curState, uint32_t newState,
                  const struct mlx_modify_qp_req *attr);
void mlx_destroy_qp(mlx_qp *qp);

/* ========== MR ========== */
mlx_mr *mlx_reg_mr(mlx_pd *pd, void *addr, uint64_t length,
                   uint32_t accessFlags, struct mlx_reg_mr_resp *out);
void mlx_dereg_mr(mlx_mr *mr);

/* ========== AH ========== */
mlx_ah *mlx_create_ah(mlx_context *ctx, const struct mlx_create_ah_req *req,
                      struct mlx_create_ah_resp *out);
void mlx_destroy_ah(mlx_ah *ah);

/* ========== Data path (zero-copy) ========== */

/* Work request types */
enum {
    MLX_WR_SEND = 0,
    MLX_WR_RDMA_WRITE = 1,
    MLX_WR_RDMA_READ = 2,
};

/* Send work request */
struct mlx_send_wr {
    uint32_t  wrType;          /* MLX_WR_* */
    uint64_t  remoteAddr;      /* for RDMA */
    uint32_t  rkey;            /* for RDMA */
    const void *data;
    uint32_t  length;
    uint32_t  lkey;            /* local MR lkey */
    uint64_t  wrId;
};

/*
 * post_send - writes WQE directly in usermode + doorbell (zero syscall)
 * See wr.c:1051 mlx5_ib_post_send
 */
int mlx_post_send(mlx_qp *qp, const struct mlx_send_wr *wr);

/*
 * post_recv - prefills RQ SGE
 * See wr.c:1220 mlx5_ib_post_recv
 */
int mlx_post_recv(mlx_qp *qp, void *buf, uint32_t length, uint32_t lkey,
                  uint64_t wrId);

/*
 * poll_cq - reads CQE buffer directly (zero syscall)
 * See cq.c:609 mlx5_ib_poll_cq
 */
int mlx_poll_cq(mlx_cq *cq, struct MlxCqe64 *cqe, int num);

/*
 * update_cq_consumer - tell the kernel the new CQ consumer index
 * The DB record page is not directly mapped to userspace; consumer
 * index updates are kernel-mediated.
 */
int mlx_update_cq_consumer(mlx_cq *cq, uint32_t consumerIndex);

/* Query completion count (used by ibv_get_cq_event) */
int mlx_query_cq_completions(mlx_context *ctx, uint32_t cqHandle,
                             uint64_t *count);

/* Get async event (non-blocking; returns EAGAIN if no event) */
int mlx_get_async_event(mlx_context *ctx, struct mlx_async_event *event);

#ifdef __cplusplus
}
#endif

#endif /* LIBMLX_H */
