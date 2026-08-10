/*
 * mlnx_qos.c — RoCE QoS configuration tool (macOS) — mlnx-tools mlnx_qos equivalent
 *
 * Configures PFC/priority mapping through ACCESS_REG.
 * Registers:
 *   - PFCC (0x500C): PFC priority configuration
 *   - QTCT (0x0A00): priority->TC mapping
 *   - QPDPM (0x5006): DSCP->priority mapping
 * Note: macOS has no sysfs/netlink DCB, writes hardware registers directly.
 */
#include "libmft.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define REG_PFCC       0x500C   /* Priority Flow Control Configuration */
#define REG_QTCT       0x0A00   /* Queue Type to TC */
#define REG_QPDPM      0x5006   /* DSCP to Priority */

static const char *g_devname = "mlx5_0";

static void
usage(void)
{
    fprintf(stderr,
        "Usage: mlnx_qos [-d <dev>] [query|-f <pfc_list>|-p <prio_tc>]\n"
        "Examples:\n"
        "  mlnx_qos query                # view current QoS configuration\n"
        "  mlnx_qos -f 0,0,0,1,0,0,0,0  # enable PFC on priority 3 (recommended for RoCE)\n"
        "  mlnx_qos -p 0,0,0,3,0,0,0,0  # map priority 3 to TC3\n"
        "Note: PFC requires switch cooperation (lossless Ethernet), RoCE v2 typically uses priority 3.\n");
}

/* Query QoS configuration */
static void
qos_query(mft_dev *dev)
{
    uint8_t data[256];
    uint32_t size = 256;
    if (mft_reg_read(dev, REG_PFCC, 0, data, &size) == 0) {
        printf("PFC configuration (PFCC register):\n");
        printf("  PFC enable bitmap: 0x%02x\n", data[0]);
        printf("  Priority [3] (RoCE): %s\n",
               (data[0] & 0x08) ? "PFC enabled" : "disabled");
    }
    if (mft_reg_read(dev, REG_QTCT, 0, data, &size) == 0) {
        printf("\nPriority->TC mapping (QTCT):\n");
        for (int i = 0; i < 8; i++)
            printf("  prio %d -> tc %d\n", i, (data[i >> 1] >> ((i & 1) ? 4 : 0)) & 0xF);
    }
    printf("\nRecommendations for RoCE v2 lossless networks:\n");
    printf("  1. Enable PFC on priority 3\n");
    printf("  2. Map priority 3 to a dedicated TC\n");
    printf("  3. Configure ECN marking on the switch\n");
}

/* Set PFC */
static void
qos_set_pfc(mft_dev *dev, const char *list)
{
    uint8_t pfc_map = 0;
    char *copy = strdup(list);
    char *tok = strtok(copy, ",");
    int idx = 0;
    while (tok && idx < 8) {
        if (atoi(tok))
            pfc_map |= (1u << idx);
        tok = strtok(NULL, ",");
        idx++;
    }
    free(copy);

    uint8_t data[256];
    memset(data, 0, sizeof(data));
    data[0] = pfc_map;
    if (mft_reg_write(dev, REG_PFCC, 0, data, 64) == 0) {
        printf("PFC configuration succeeded: enable bitmap 0x%02x\n", pfc_map);
        printf("  Priority [3] (RoCE): %s\n",
               (pfc_map & 0x08) ? "PFC enabled" : "disabled");
        printf("  Note: PFC must be configured on the switch as well\n");
    } else {
        printf("Error: PFC configuration failed\n");
    }
}

/* Set priority->TC mapping */
static void
qos_set_prio_tc(mft_dev *dev, const char *list)
{
    uint8_t qtct[8] = {0};
    char *copy = strdup(list);
    char *tok = strtok(copy, ",");
    int idx = 0;
    while (tok && idx < 8) {
        int tc = atoi(tok);
        if (idx & 1)
            qtct[idx >> 1] |= (uint8_t)((tc & 0xF) << 4);
        else
            qtct[idx >> 1] |= (uint8_t)(tc & 0xF);
        tok = strtok(NULL, ",");
        idx++;
    }
    free(copy);

    if (mft_reg_write(dev, REG_QTCT, 0, qtct, 8) == 0) {
        printf("Priority->TC mapping configured successfully\n");
        for (int i = 0; i < 8; i++)
            printf("  prio %d -> tc %d\n", i, (qtct[i >> 1] >> ((i & 1) ? 4 : 0)) & 0xF);
    } else {
        printf("Error: QTCT configuration failed\n");
    }
}

int main(int argc, char **argv)
{
    const char *pfc = NULL;
    const char *prio_tc = NULL;
    int do_query = 0;
    int i = 1;

    while (i < argc) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            g_devname = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            pfc = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            prio_tc = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "query") == 0) {
            do_query = 1;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (!pfc && !prio_tc)
        do_query = 1;

    mft_dev *dev = mft_open(g_devname);
    if (!dev) {
        printf("Error: cannot open device %s\n", g_devname);
        return 1;
    }

    if (do_query)
        qos_query(dev);
    if (pfc)
        qos_set_pfc(dev, pfc);
    if (prio_tc)
        qos_set_prio_tc(dev, prio_tc);

    mft_close(dev);
    return 0;
}
