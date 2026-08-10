/*
 * MlxGID.hpp — GID table management (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/main.c:577 (set_roce_addr) + :617 (mlx5_ib_add_gid)
 * macOS difference: userspace policy supplies address changes to the driver.
 */
#ifndef MLX_GID_HPP
#define MLX_GID_HPP

#include <libkern/OSTypes.h>
#include "MlxRegs.hpp"

class MlxRoCE;
class MlxPCIDriver;

/*
 * GID entry
 */
struct MlxGIDEntry {
    bool        used;
    uint32_t    index;
    uint8_t     gid[16];        /* IP (IPv4-mapped or IPv6) */
    uint8_t     mac[6];         /* corresponding MAC */
    uint8_t     roceVersion;    /* 2 = RoCEv2 */
    uint8_t     l3Type;         /* 1=IPv4 2=IPv6 */
    uint16_t    vlanId;
    bool        vlanEn;
};

/*
 * GID table management class
 */
class MlxGID : public OSObject {
    OSDeclareDefaultStructors(MlxGID)

public:
    bool init(MlxRoCE *roce, uint32_t tableSize);
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* Allocate a free index (see ib core GID table allocation) */
    uint32_t allocGIDIndex();
    void freeGIDIndex(uint32_t index);

    /* Write to firmware (see set_roce_addr, main.c:577 → mlx5_core_roce_gid_set) */
    kern_return_t setGID(uint32_t index, const uint8_t *gid,
                         const uint8_t *mac, uint8_t roceVersion,
                         uint8_t l3Type, bool vlanEn, uint16_t vlanId);
    kern_return_t delGID(uint32_t index);

    /* macOS-specific: monitor IP address changes → automatically write the firmware GID table */
    void startAddressMonitor();
    void stopAddressMonitor();

    /* Current interface IP (for peer queries) */
    kern_return_t getLocalAddr(uint8_t *gid, uint8_t *mac);

private:
    /* SET_ROCE_ADDRESS command wrapper */
    kern_return_t cmdSetRoceAddr(uint32_t index, const uint8_t *gid,
                                 const uint8_t *mac, uint8_t version,
                                 uint8_t l3Type, bool vlanEn, uint16_t vlanId);

    MlxRoCE         *fRoce;
    MlxPCIDriver    *fCore;
    MlxGIDEntry     *fTable;
    uint32_t         fTableSize;
    bool            *fUsed;
    IOLock          *fLock;
    bool             fMonitoring;
};

#endif /* MLX_GID_HPP */
