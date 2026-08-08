/*
 * ibv_devinfo.c - device info inspection tool (See Linux ibv_devinfo)
 *
 * Verification: enumerate all MlxRoCE devices → query capabilities one by one → print RoCE info
 */
#include "libmlx.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    /* Enumerate all devices (multi-device support) */
    char *names[8] = {0};
    int count = mlx_list_devices(names, 8);
    if (count <= 0) {
        printf("Error: no MlxRoCE device found (driver not loaded?)\n");
        return 1;
    }
    printf("Found %d RDMA device(s):\n", count);

    for (int i = 0; i < count; i++) {
        mlx_context *ctx = mlx_open_device_by_name(names[i]);
        if (!ctx) {
            printf("  [%d] %s: open failed\n", i, names[i]);
            continue;
        }

        struct mlx_query_device_resp dev = {};
        struct mlx_query_port_resp port = {};

        printf("\nDevice [%d] %s:\n", i, names[i]);
        if (mlx_query_device(ctx, &dev) != 0) {
            printf("  Error: query device failed\n");
            mlx_close_device(ctx);
            continue;
        }
        if (mlx_query_port(ctx, &port) != 0) {
            printf("  Error: query port failed\n");
            mlx_close_device(ctx);
            continue;
        }

        printf("  device_id: %04x, fw_version: %08llx\n",
               dev.deviceId, dev.fwVersion);
        printf("  num_ports: %u, max_qp: %u, max_cq: %u, max_mr: %u\n",
               dev.numPorts, dev.maxQp, dev.maxCq, dev.maxMr);
        printf("  RoCE: v1=%s v2=%s, max_gid=%u\n",
               (dev.roceVersions & 0x1) ? "yes" : "no",
               (dev.roceVersions & 0x2) ? "yes" : "no",
               dev.maxGid);
        printf("  port 1: %s, %u Mbps, %s\n",
               port.portState ? "PORT_ACTIVE" : "PORT_DOWN",
               port.activeSpeed,
               port.gidType == 2 ? "RoCE v2" : "RoCE v1");

        mlx_close_device(ctx);
        free(names[i]);
    }

    return 0;
}
