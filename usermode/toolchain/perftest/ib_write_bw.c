/*
 * ib_write_bw.c — RDMA Write bandwidth test (macOS) — perftest equivalent
 *
 * Simplified implementation based on the libverbs compatibility layer (verbs.h).
 * Usage: ib_write_bw [server|client]
 *   - server: ib_write_bw
 *   - client: ib_write_bw <server_ip>
 *
 * Real TCP handshake: both ends exchange QPN/RKEY/PSN/GID, then enter the RDMA Write loop.
 */
#include "verbs.h"
#include "perftest_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

int main(int argc, char **argv)
{
    struct pt_ctx ctx;
    if (pt_init(&ctx, argc, argv) != 0)
        return 1;
    printf("ib_write_bw (macOS) - RDMA Write bandwidth test\n");
    printf("  Mode: %s\n", ctx.cfg.is_server ? "server (waiting for connection)" : "client");
    printf("  Size: %zu bytes, %d iterations\n", ctx.cfg.size, ctx.cfg.iters);

    if (pt_create_qp(&ctx) != 0)
        return 1;

    /* Real TCP handshake: exchange QPN/RKEY/PSN/GID */
    struct pt_peer peer;
    if (pt_handshake(&ctx, &peer) != 0)
        return 1;

    /* QP state machine: RST->INIT->RTR->RTS (using peer parameters) */
    if (pt_qp_to_rts(&ctx, &peer) != 0)
        return 1;

    /* RDMA Write loop: local buf -> peer remote_addr (peer RKEY) */
    double t0 = pt_now();
    for (int i = 0; i < ctx.cfg.iters; i++) {
        struct ibv_sge sge;
        memset(&sge, 0, sizeof(sge));
        sge.addr = (uint64_t)(uintptr_t)ctx.buf;
        sge.length = (uint32_t)ctx.cfg.size;
        sge.lkey = ctx.mr->lkey;

        struct ibv_send_wr wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = (uint64_t)i;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_GENERATE_COMPLETION;
        wr.wr.rdma.remote_addr = (uint64_t)(uintptr_t)ctx.buf;
        wr.wr.rdma.rkey = peer.rkey;

        struct ibv_send_wr *bad;
        if (ibv_post_send(ctx.qp, &wr, &bad) != 0) {
            printf("Error: post_send failed (iteration %d)\n", i);
            return 1;
        }
    }
    double t1 = pt_now();

    pt_result(&ctx, t1 - t0);
    pt_cleanup(&ctx);
    return 0;
}
