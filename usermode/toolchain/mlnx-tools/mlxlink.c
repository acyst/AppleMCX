/*
 * mlxlink.c — link/port diagnostics tool (macOS) — mft mlxlink equivalent
 *
 * Displays port link state, speed, statistics (based on libmft port_stats)
 */
#include "libmft.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *devname = (argc > 1) ? argv[1] : "mlx5_0";

    mft_dev *dev = mft_open(devname);
    if (!dev) {
        printf("Error: cannot open device %s\n", devname);
        return 1;
    }

    struct mft_port_stats st;
    uint32_t fw = 0, cmdif = 0;

    printf("mlxlink - link diagnostics (macOS)\n");
    printf("Device: %s\n", devname);
    if (mft_query_fw_ver(dev, &fw, &cmdif) == 0)
        printf("Firmware: %u.%u.%u\n", (fw >> 24) & 0xFF,
               (fw >> 16) & 0xFF, (fw >> 8) & 0xFF);

    if (mft_port_stats(dev, &st) != 0) {
        printf("Error: failed to read port statistics\n");
        mft_close(dev);
        return 1;
    }

    printf("\nPort %u:\n", st.port_num);
    printf("  Link state: %s\n", st.link_state ? "ACTIVE" : "DOWN");
    printf("  Link speed: %u Mbps\n", st.link_speed);
    printf("  RX stats: %llu pkts / %llu bytes\n", st.rx_pkts, st.rx_bytes);
    printf("  TX stats: %llu pkts / %llu bytes\n", st.tx_pkts, st.tx_bytes);
    printf("  RX drops: %llu, TX drops: %llu\n", st.rx_drop, st.tx_drop);
    printf("  RX errors: %llu, TX errors: %llu\n", st.rx_errors, st.tx_errors);

    mft_close(dev);
    return 0;
}
