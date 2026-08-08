/*
 * mlxreg.c — register read/write tool (macOS) — mft mlxreg equivalent
 *
 * Usage:
 *   mlxreg read <reg_id> [arg]     # read register
 *   mlxreg write <reg_id> <data>   # write register (data is hexadecimal)
 * Register IDs use hexadecimal (e.g. 0x500C = PFCC)
 */
#include "libmft.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void
usage(void)
{
    fprintf(stderr,
        "Usage: mlxreg [-d <dev>] read|write <reg_id_hex> [data_hex]\n"
        "Examples:\n"
        "  mlxreg read 0x500c          # read PFCC (PFC configuration)\n"
        "  mlxreg write 0x500c 08      # write PFCC\n"
        "Common registers:\n"
        "  0x500C PFCC  PFC configuration\n"
        "  0x0A00 QTCT  priority->TC\n"
        "  0x5006 QPDPM DSCP->priority\n"
        "  0x9005 MHCR  HCA configuration\n");
}

int main(int argc, char **argv)
{
    const char *devname = "mlx5_0";
    int i = 1;
    if (argc > 2 && strcmp(argv[1], "-d") == 0) {
        devname = argv[2];
        i = 3;
    }
    if (argc <= i) {
        usage();
        return 1;
    }

    const char *op = argv[i];
    const char *regStr = argv[i + 1];
    if (!regStr) {
        usage();
        return 1;
    }
    uint32_t regId = (uint32_t)strtoul(regStr, NULL, 0);

    mft_dev *dev = mft_open(devname);
    if (!dev) {
        printf("Error: cannot open device %s\n", devname);
        return 1;
    }

    if (strcmp(op, "read") == 0) {
        uint32_t arg = (i + 2 < argc) ? (uint32_t)strtoul(argv[i + 2], NULL, 0) : 0;
        uint8_t data[256];
        uint32_t size = 256;
        if (mft_reg_read(dev, regId, arg, data, &size) != 0) {
            printf("Error: register 0x%04x read failed\n", regId);
            mft_close(dev);
            return 1;
        }
        printf("Register 0x%04x (read):\n", regId);
        printf("  Data size: %u bytes\n", size);
        for (uint32_t j = 0; j < size; j += 16) {
            printf("  %04x: ", j);
            for (uint32_t k = 0; k < 16 && j + k < size; k++)
                printf("%02x ", data[j + k]);
            printf("\n");
        }
    } else if (strcmp(op, "write") == 0 && i + 2 < argc) {
        /* Parse data from a hexadecimal string */
        const char *hex = argv[i + 2];
        uint8_t data[256];
        uint32_t size = 0;
        size_t hexLen = strlen(hex);
        for (size_t j = 0; j + 1 < hexLen && size < 256; j += 2) {
            char byte[3] = {hex[j], hex[j + 1], 0};
            data[size++] = (uint8_t)strtoul(byte, NULL, 16);
        }
        if (mft_reg_write(dev, regId, 0, data, size) != 0) {
            printf("Error: register 0x%04x write failed\n", regId);
            mft_close(dev);
            return 1;
        }
        printf("Register 0x%04x wrote %u bytes\n", regId, size);
    } else {
        usage();
        mft_close(dev);
        return 1;
    }

    mft_close(dev);
    return 0;
}
