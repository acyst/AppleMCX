/*
 * mlxstatus.c — device status inspection tool (macOS) — mft mlxstatus equivalent
 *
 * Displays: device name/firmware version/command interface version/health status/port statistics
 * Based on libmft (firmware management library)
 */
#include "libmft.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void
print_device(const char *devname)
{
    mft_dev *dev = mft_open(devname);
    if (!dev) {
        printf("%-12s open failed\n", devname);
        return;
    }

    uint32_t fw = 0, cmdif = 0;
    uint32_t healthy = 0;
    struct mft_port_stats st;

    printf("Device: %s\n", devname);
    if (mft_query_fw_ver(dev, &fw, &cmdif) == 0) {
        printf("  Firmware Version: %u.%u.%u\n",
               (fw >> 24) & 0xFF, (fw >> 16) & 0xFF, (fw >> 8) & 0xFF);
        printf("  Command Interface Rev: %u\n", cmdif);
    }
    if (mft_health(dev, &healthy) == 0) {
        printf("  Health Status: %s\n", healthy ? "OK" : "DEGRADED");
    }
    if (mft_port_stats(dev, &st) == 0) {
        printf("  Port %u: %s, %u Mbps\n",
               st.port_num, st.link_state ? "ACTIVE" : "DOWN",
               st.link_speed);
        printf("  RX: %llu pkts, %llu bytes, %llu errors\n",
               st.rx_pkts, st.rx_bytes, st.rx_errors);
        printf("  TX: %llu pkts, %llu bytes, %llu errors\n",
               st.tx_pkts, st.tx_bytes, st.tx_errors);
    }
    printf("\n");

    mft_close(dev);
}

int main(int argc, char **argv)
{
    /* Args: none -> default mlx5_0; -a -> all */
    int all = 0;
    if (argc > 1 && strcmp(argv[1], "-a") == 0)
        all = 1;

    if (all) {
        /* All devices: enumerate (MVP uses a fixed list, see mlx_list_devices for multi-device enumeration) */
        printf("=== All devices status ===\n\n");
        /* Try common device names */
        for (int i = 0; i < 8; i++) {
            char name[16];
            snprintf(name, sizeof(name), "mlx5_%d", i);
            mft_dev *dev = mft_open(name);
            if (!dev)
                break;   /* no more devices */
            mft_close(dev);
            print_device(name);
        }
    } else {
        const char *devname = (argc > 1) ? argv[1] : "mlx5_0";
        print_device(devname);
    }
    return 0;
}
