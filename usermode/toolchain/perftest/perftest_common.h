/*
 * perftest_common.h — perftest tools common framework (macOS)
 *
 * Provides the common flow for RDMA tests: device open/PD/MR/CQ/QP creation + result output.
 * Reused by ib_write_bw / ib_send_bw / ib_read_bw.
 */
#ifndef PERFTEST_COMMON_H
#define PERFTEST_COMMON_H

#include "verbs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PT_BUF_SIZE      (1 << 20)   /* 1 MB */
#define PT_ITERATIONS    1000
#define PT_PORT          18515       /* perftest TCP handshake port */
#define PT_HS_VER        1

/* Handshake message: both ends exchange QP connection parameters (see perftest's TCP msg exchange) */
struct pt_hs_msg {
    uint32_t qpn;
    uint32_t rkey;
    uint32_t psn;
    uint8_t  gid[16];
    uint32_t size;
    uint32_t iters;
    uint32_t ver;
};

/* Peer parameters (obtained during handshake) */
struct pt_peer {
    uint32_t qpn;
    uint32_t rkey;
    uint32_t psn;
    uint8_t  gid[16];
    struct sockaddr_in addr;
};

/* Test configuration */
struct pt_config {
    int   is_server;
    int   iters;
    size_t size;
    char  server_ip[64];
};

/* Test context */
struct pt_ctx {
    struct ibv_context *ibv;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
    struct pt_config cfg;
};

/* Initialize test context */
static inline int
pt_init(struct pt_ctx *ctx, int argc, char **argv)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg.is_server = (argc < 2);
    if (!ctx->cfg.is_server && argc >= 2)
        strncpy(ctx->cfg.server_ip, argv[1], sizeof(ctx->cfg.server_ip) - 1);
    ctx->cfg.iters = PT_ITERATIONS;
    ctx->cfg.size = PT_BUF_SIZE;

    struct ibv_device **devs = ibv_get_device_list(NULL);
    if (!devs) {
        printf("Error: no RDMA device (driver not loaded?)\n");
        return -1;
    }
    ctx->ibv = ibv_open_device(devs[0]);
    if (!ctx->ibv) {
        printf("Error: cannot open device\n");
        return -1;
    }
    struct ibv_device_attr attr;
    if (ibv_query_device(ctx->ibv, &attr) != 0) {
        printf("Error: query device failed\n");
        return -1;
    }
    printf("Device: %s (fw=%llu, qp=%d)\n",
           ibv_get_device_name(devs[0]), attr.fw_ver, attr.max_qp);

    ctx->pd = ibv_alloc_pd(ctx->ibv);
    if (!ctx->pd) {
        printf("Error: PD allocation failed\n");
        return -1;
    }
    if (posix_memalign((void **)&ctx->buf, 4096, ctx->cfg.size) != 0) {
        printf("Error: buffer allocation failed\n");
        return -1;
    }
    memset(ctx->buf, 0xab, ctx->cfg.size);
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->cfg.size,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
    if (!ctx->mr) {
        printf("Error: MR registration failed\n");
        return -1;
    }
    ctx->cq = ibv_create_cq(ctx->ibv, 1024, NULL, NULL, 0);
    if (!ctx->cq) {
        printf("Error: CQ creation failed\n");
        return -1;
    }
    return 0;
}

/* Create RC QP */
static inline int
pt_create_qp(struct pt_ctx *ctx)
{
    struct ibv_qp_init_attr init;
    memset(&init, 0, sizeof(init));
    init.send_cq = ctx->cq;
    init.recv_cq = ctx->cq;
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 128;
    init.cap.max_recv_wr = 128;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    ctx->qp = ibv_create_qp(ctx->pd, &init);
    if (!ctx->qp) {
        printf("Error: QP creation failed\n");
        return -1;
    }
    return 0;
}

/* QP state machine: RST->INIT->RTR->RTS (real state machine, RTR/RTS use peer parameters obtained from the handshake) */
static inline int
pt_qp_to_rts(struct pt_ctx *ctx, const struct pt_peer *peer)
{
    struct ibv_qp_attr attr;
    int mask;

    /* RST → INIT */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = 1;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    mask = IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(ctx->qp, &attr, mask) != 0) {
        printf("Error: RST->INIT failed\n");
        return -1;
    }

    /* INIT -> RTR: use peer QPN/PSN, destination GID = peer->gid */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = peer->qpn;
    attr.rq_psn = peer->psn;
    attr.max_dest_rd_atomic = 16;
    attr.min_rnr_timer = 12;
    /* AH: peer GID as the destination GID (RoCE path) */
    memset(&attr.ah_attr, 0, sizeof(attr.ah_attr));
    memcpy(attr.ah_attr.grh.dgid.raw, peer->gid, 16);
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit = 64;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = 1;
    mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
           IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
           IBV_QP_AV;
    if (ibv_modify_qp(ctx->qp, &attr, mask) != 0) {
        printf("Error: INIT->RTR failed\n");
        return -1;
    }

    /* RTR → RTS */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 16;
    mask = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
           IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(ctx->qp, &attr, mask) != 0) {
        printf("Error: RTR->RTS failed\n");
        return -1;
    }
    return 0;
}

/* Real TCP handshake: exchange QP connection parameters (qpn/rkey/psn/gid) */
static inline int
pt_handshake(struct pt_ctx *ctx, struct pt_peer *peer)
{
    int sock = -1, listen_sock = -1;
    struct sockaddr_in addr;
    struct pt_hs_msg msg, remote;

    /* Fill in local parameters */
    memset(&msg, 0, sizeof(msg));
    msg.qpn = ctx->qp->qp_num;
    msg.rkey = ctx->mr->rkey;
    msg.psn = 0;   /* sq_psn start */
    msg.size = (uint32_t)ctx->cfg.size;
    msg.iters = (uint32_t)ctx->cfg.iters;
    msg.ver = PT_HS_VER;
    memset(msg.gid, 0, sizeof(msg.gid));
    {
        union ibv_gid gid;
        if (ibv_query_gid(ctx->ibv, 1, 0, &gid) == 0)
            memcpy(msg.gid, gid.raw, 16);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PT_PORT);

    if (ctx->cfg.is_server) {
        /* Server: listen + accept */
        listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock < 0) {
            perror("socket");
            return -1;
        }
        int one = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind");
            close(listen_sock);
            return -1;
        }
        if (listen(listen_sock, 1) < 0) {
            perror("listen");
            close(listen_sock);
            return -1;
        }
        printf("Server ready, waiting for client connection (port %d)...\n", PT_PORT);
        socklen_t alen = sizeof(peer->addr);
        sock = accept(listen_sock, (struct sockaddr *)&peer->addr, &alen);
        if (sock < 0) {
            perror("accept");
            close(listen_sock);
            return -1;
        }
        close(listen_sock);
        printf("Client connected: %s\n",
               inet_ntoa(peer->addr.sin_addr));
    } else {
        /* Client: connect */
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            return -1;
        }
        if (inet_pton(AF_INET, ctx->cfg.server_ip, &addr.sin_addr) <= 0) {
            printf("Error: invalid server address %s\n", ctx->cfg.server_ip);
            close(sock);
            return -1;
        }
        printf("Client: connecting to server %s:%d...\n",
               ctx->cfg.server_ip, PT_PORT);
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(sock);
            return -1;
        }
    }

    /* Exchange parameters: send local first, then receive peer */
    if (send(sock, &msg, sizeof(msg), 0) != (ssize_t)sizeof(msg)) {
        printf("Error: handshake send failed\n");
        close(sock);
        return -1;
    }
    if (recv(sock, &remote, sizeof(remote), 0) != (ssize_t)sizeof(remote)) {
        printf("Error: handshake receive failed\n");
        close(sock);
        return -1;
    }
    close(sock);

    /* Validate protocol version and parameters */
    if (remote.ver != PT_HS_VER) {
        printf("Error: peer handshake version incompatible (%u vs %u)\n", remote.ver, PT_HS_VER);
        return -1;
    }
    if (remote.size != ctx->cfg.size) {
        printf("Error: peer buffer size mismatch (%u vs %zu)\n",
               remote.size, ctx->cfg.size);
        return -1;
    }
    if (remote.iters != ctx->cfg.iters) {
        printf("Error: peer iteration count mismatch (%u vs %d)\n",
               remote.iters, ctx->cfg.iters);
        return -1;
    }

    peer->qpn = remote.qpn;
    peer->rkey = remote.rkey;
    peer->psn = remote.psn;
    memcpy(peer->gid, remote.gid, 16);
    printf("Handshake succeeded: peer QPN=%u RKEY=0x%x PSN=%u\n",
           peer->qpn, peer->rkey, peer->psn);
    return 0;
}

/* Print result */
static inline void
pt_result(struct pt_ctx *ctx, double secs)
{
    double bw = (double)ctx->cfg.size * ctx->cfg.iters / secs / 1e6;
    printf("\nResults:\n");
    printf("  Transferred: %d x %zu bytes = %.2f MB\n", ctx->cfg.iters,
           ctx->cfg.size, (double)ctx->cfg.size * ctx->cfg.iters / 1e6);
    printf("  Time: %.3f sec\n", secs);
    printf("  Bandwidth: %.2f MB/s (%.2f Gb/s)\n", bw, bw * 8 / 1000);
}

/* Cleanup */
static inline void
pt_cleanup(struct pt_ctx *ctx)
{
    if (ctx->qp) ibv_destroy_qp(ctx->qp);
    if (ctx->cq) ibv_destroy_cq(ctx->cq);
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    if (ctx->ibv) ibv_close_device(ctx->ibv);
    ibv_free_device_list(NULL);
    free(ctx->buf);
}

/* Time */
static inline double
pt_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#endif /* PERFTEST_COMMON_H */
