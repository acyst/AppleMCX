/*
 * ib_send_lat.c — SEND latency test (macOS) — perftest equivalent
 * Usage: ib_send_lat [server|client]
 */
#include "perftest_lat.h"

int main(int argc, char **argv)
{
    struct pt_ctx ctx;
    if (pt_init(&ctx, argc, argv) != 0)
        return 1;
    printf("ib_send_lat (macOS) - SEND latency test\n");
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

    struct pt_lat_stats stats;
    lat_init(&stats);

    for (int i = 0; i < ctx.cfg.iters; i++) {
        struct ibv_sge sge;
        memset(&sge, 0, sizeof(sge));
        sge.addr = (uint64_t)(uintptr_t)ctx.buf;
        sge.length = ctx.cfg.size;
        sge.lkey = ctx.mr->lkey;

        struct ibv_send_wr wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = i;
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_GENERATE_COMPLETION;

        double t0 = pt_now();
        struct ibv_send_wr *bad;
        ibv_post_send(ctx.qp, &wr, &bad);
        double t1 = pt_now();
        lat_add(&stats, (t1 - t0) * 1e6);
    }

    lat_print("ib_send_lat", &stats);
    pt_cleanup(&ctx);
    return 0;
}
