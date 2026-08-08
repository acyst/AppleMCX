/*
 * ib_read_bw.c — RDMA Read bandwidth test (macOS) — perftest equivalent
 * Usage: ib_read_bw [server|client]
 */
#include "perftest_common.h"

int main(int argc, char **argv)
{
    struct pt_ctx ctx;
    if (pt_init(&ctx, argc, argv) != 0)
        return 1;
    printf("ib_read_bw (macOS) - RDMA Read bandwidth test\n");
    printf("  Mode: %s, size: %zu, %d iterations\n",
           ctx.cfg.is_server ? "server" : "client",
           ctx.cfg.size, ctx.cfg.iters);

    if (pt_create_qp(&ctx) != 0)
        return 1;
    struct pt_peer peer;
    if (pt_handshake(&ctx, &peer) != 0)
        return 1;
    if (pt_qp_to_rts(&ctx, &peer) != 0)
        return 1;

    double t0 = pt_now();
    for (int i = 0; i < ctx.cfg.iters; i++) {
        struct ibv_sge sge;
        memset(&sge, 0, sizeof(sge));
        sge.addr = (uint64_t)(uintptr_t)ctx.buf;
        sge.length = ctx.cfg.size;
        sge.lkey = ctx.mr->lkey;

        struct ibv_send_wr wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = i;
        wr.opcode = IBV_WR_RDMA_READ;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_GENERATE_COMPLETION;
        wr.wr.rdma.remote_addr = (uint64_t)(uintptr_t)ctx.buf;
        wr.wr.rdma.rkey = peer.rkey;
        struct ibv_send_wr *bad;
        ibv_post_send(ctx.qp, &wr, &bad);
    }
    double t1 = pt_now();

    pt_result(&ctx, t1 - t0);
    pt_cleanup(&ctx);
    return 0;
}
