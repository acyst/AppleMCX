/*
 * libmft.h — firmware management library (macOS) — mstflint/mft equivalent
 *
 * Provided through MlxUserClient's firmware management methods:
 *   - firmware version query
 *   - register read/write (ACCESS_REG) -> mlxconfig equivalent
 *   - firmware command passthrough -> mlxup equivalent
 *   - port statistics -> mlxlink equivalent
 */
#ifndef LIBMFT_H
#define LIBMFT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Device handle */
typedef struct mft_dev mft_dev;

/* Open/close device */
mft_dev *mft_open(const char *devname);
void mft_close(mft_dev *dev);

/* Firmware version */
int mft_query_fw_ver(mft_dev *dev, uint32_t *fw_rev,
                     uint32_t *cmdif_rev);

/* Register read/write (ACCESS_REG) */
int mft_reg_read(mft_dev *dev, uint32_t reg_id, uint32_t arg,
                 uint8_t *data, uint32_t *size);
int mft_reg_write(mft_dev *dev, uint32_t reg_id, uint32_t arg,
                  const uint8_t *data, uint32_t size);

/* Arbitrary firmware command passthrough */
int mft_fw_cmd(mft_dev *dev, uint16_t opcode, uint16_t op_mod,
               const uint8_t *in, uint32_t in_size,
               uint8_t *out, uint32_t *out_size);

/* Port statistics */
int mft_port_stats(mft_dev *dev, struct mft_port_stats *stats);

struct mft_port_stats {
    uint64_t rx_pkts, tx_pkts, rx_bytes, tx_bytes;
    uint64_t rx_drop, tx_drop, rx_errors, tx_errors;
    uint32_t link_speed;
    uint8_t  link_state;
    uint8_t  port_num;
};

/* Health status */
int mft_health(mft_dev *dev, uint32_t *healthy);

#ifdef __cplusplus
}
#endif

#endif /* LIBMFT_H */
