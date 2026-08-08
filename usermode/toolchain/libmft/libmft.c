/*
 * libmft.c — firmware management library implementation (macOS)
 *
 * Wraps the firmware tools API through MlxUserClient's firmware management methods.
 * Underlying: IOConnectCallStructMethod (kMlxUCMethod*)
 */
#include "libmft.h"
#include "MlxUCIO.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __APPLE__
#include <IOKit/IOKitLib.h>
#endif

struct mft_dev {
    io_connect_t conn;
    char devname[64];
};

mft_dev *mft_open(const char *devname)
{
    mft_dev *dev = (mft_dev *)calloc(1, sizeof(mft_dev));
    if (!dev)
        return NULL;
    if (devname)
        strncpy(dev->devname, devname, sizeof(dev->devname) - 1);
    else
        strncpy(dev->devname, "mlx5_0", sizeof(dev->devname) - 1);

#ifdef __APPLE__
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("MlxRoCE"));
    if (!svc) {
        free(dev);
        return NULL;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &dev->conn);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) {
        free(dev);
        return NULL;
    }
#endif
    return dev;
}

void mft_close(mft_dev *dev)
{
    if (!dev)
        return;
#ifdef __APPLE__
    if (dev->conn)
        IOServiceClose(dev->conn);
#endif
    free(dev);
}

int mft_query_fw_ver(mft_dev *dev, uint32_t *fw_rev, uint32_t *cmdif_rev)
{
    if (!dev || !fw_rev)
        return -1;
    struct mlx_fw_ver_resp resp;
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryFwVer, NULL, 0, &resp, &outSize);
    if (kr != kIOReturnSuccess)
        return -1;
    *fw_rev = resp.fwRev;
    if (cmdif_rev)
        *cmdif_rev = resp.cmdifRev;
    return 0;
}

int mft_reg_read(mft_dev *dev, uint32_t reg_id, uint32_t arg,
                 uint8_t *data, uint32_t *size)
{
    if (!dev || !data || !size)
        return -1;
    struct mlx_access_reg_req req;
    struct mlx_access_reg_resp resp;
    memset(&req, 0, sizeof(req));
    req.registerId = reg_id;
    req.opMod = 0;              /* read */
    req.argument = arg;
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodAccessReg, &req, sizeof(req), &resp, &outSize);
    if (kr != kIOReturnSuccess)
        return -1;
    uint32_t copySz = (resp.dataSize < *size) ? resp.dataSize : *size;
    memcpy(data, resp.data, copySz);
    *size = copySz;
    return 0;
}

int mft_reg_write(mft_dev *dev, uint32_t reg_id, uint32_t arg,
                  const uint8_t *data, uint32_t size)
{
    if (!dev || !data)
        return -1;
    struct mlx_access_reg_req req;
    memset(&req, 0, sizeof(req));
    req.registerId = reg_id;
    req.opMod = 1;              /* write */
    req.argument = arg;
    memcpy(req.data, data, (size < 240) ? size : 240);
    req.dataSize = size;
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodAccessReg, &req, sizeof(req), NULL, 0);
    return kr == kIOReturnSuccess ? 0 : -1;
}

int mft_fw_cmd(mft_dev *dev, uint16_t opcode, uint16_t op_mod,
               const uint8_t *in, uint32_t in_size,
               uint8_t *out, uint32_t *out_size)
{
    if (!dev || !in || !out || !out_size)
        return -1;
    struct mlx_fw_cmd_req req;
    struct mlx_fw_cmd_resp resp;
    memset(&req, 0, sizeof(req));
    req.opcode = opcode;
    req.opMod = op_mod;
    memcpy(req.in, in, (in_size < 512) ? in_size : 512);
    req.inSize = in_size;
    size_t outSz = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodFwCmd, &req, sizeof(req), &resp, &outSz);
    if (kr != kIOReturnSuccess)
        return -1;
    uint32_t copySz = (resp.outSize < *out_size) ? resp.outSize : *out_size;
    memcpy(out, resp.out, copySz);
    *out_size = copySz;
    return 0;
}

int mft_port_stats(mft_dev *dev, struct mft_port_stats *stats)
{
    if (!dev || !stats)
        return -1;
    struct mlx_port_stats_resp resp;
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodPortStats, NULL, 0, &resp, &outSize);
    if (kr != kIOReturnSuccess)
        return -1;
    memset(stats, 0, sizeof(*stats));
    stats->rx_pkts = resp.rxPkts;
    stats->tx_pkts = resp.txPkts;
    stats->rx_bytes = resp.rxBytes;
    stats->tx_bytes = resp.txBytes;
    stats->rx_drop = resp.rxDrop;
    stats->tx_drop = resp.txDrop;
    stats->rx_errors = resp.rxErrors;
    stats->tx_errors = resp.txErrors;
    stats->link_speed = resp.linkSpeed;
    stats->link_state = resp.linkState;
    stats->port_num = resp.portNum;
    return 0;
}

int mft_health(mft_dev *dev, uint32_t *healthy)
{
    if (!dev || !healthy)
        return -1;
    size_t outSize = 4;
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryHealth, NULL, 0, healthy, &outSize);
    return kr == kIOReturnSuccess ? 0 : -1;
}
