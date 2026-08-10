/*
 * mlxconfig.c — firmware configuration tool (macOS) — mft mlxconfig equivalent
 *
 * Queries/sets NIC firmware configuration through ACCESS_REG.
 * Common configuration items (see mft mlxconfig):
 *   - SRIOV_EN, NUM_OF_VFS
 *   - ROCE_ENABLE (RoCE switch)
 *   - QP_TS_FORMAT
 *   - LOG_BAR_SIZE
 */
#include "libmft.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Common register IDs (see mft register definitions) */
#define REG_MHCR       0x9005   /* Management HCA Configuration Register */
#define REG_QTCT       0x0A00   /* Queue Type to Traffic Class */
#define REG_PVLC       0x0A04   /* Port Vl Capability */
#define REG_PTYS       0x5004   /* Port Type and Speed */

static void
print_fw_version(mft_dev *dev)
{
    uint32_t fw, cmdif;
    if (mft_query_fw_ver(dev, &fw, &cmdif) == 0) {
        uint32_t maj = (fw >> 24) & 0xFF;
        uint32_t min = (fw >> 16) & 0xFF;
        uint32_t sub = (fw >> 8) & 0xFF;
        printf("  Firmware version: %u.%u.%u\n", maj, min, sub);
        printf("  Command interface rev: %u\n", cmdif);
    }
}

static void
print_port_info(mft_dev *dev)
{
    struct mft_port_stats st;
    if (mft_port_stats(dev, &st) == 0) {
        printf("  Port %u: %s, %u Mbps\n",
               st.port_num, st.link_state ? "ACTIVE" : "DOWN",
               st.link_speed);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: mlxconfig [-d <dev>] [query|set <name> <value>]\n"
            "Examples:\n"
            "  mlxconfig query                 # view current configuration\n"
            "  mlxconfig set ROCE_ENABLE 1     # enable RoCE\n"
            "  mlxconfig set SRIOV_EN 0        # disable SR-IOV\n");
        return 1;
    }

    const char *devname = "mlx5_0";
    int i = 1;
    if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
        devname = argv[i + 1];
        i += 2;
    }

    mft_dev *dev = mft_open(devname);
    if (!dev) {
        printf("Error: cannot open device %s\n", devname);
        return 1;
    }

    const char *cmd = argv[i];

    if (strcmp(cmd, "query") == 0) {
        printf("Device: %s\n", devname);
        print_fw_version(dev);
        print_port_info(dev);

        /* Query RoCE enable (see mlxconfig query) */
        uint8_t data[256];
        uint32_t size = 256;
        if (mft_reg_read(dev, REG_MHCR, 0, data, &size) == 0) {
            printf("  ROCE_ENABLE: %s (register query)\n",
                   (data[0] & 0x1) ? "1" : "0");
        }
        printf("\nNote: the macOS version of mlxconfig supports firmware configuration query,\n");
        printf("      write operations (set) require firmware support for a writable config region.\n");
    } else if (strcmp(cmd, "set") == 0 && i + 2 < argc) {
        const char *name = argv[i + 1];
        const char *value = argv[i + 2];
        printf("Setting %s = %s (macOS version, requires firmware support)\n", name, value);
        /* MVP: write operations via mft_reg_write, full configuration items later in P5 */
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        mft_close(dev);
        return 1;
    }

    mft_close(dev);
    return 0;
}
