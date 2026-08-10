/*
 * MlxGID.cpp — GID table management implementation (generic Mellanox family)
 *
 * Ported from: drivers/net/ethernet/mellanox/mlx5/core/lib/gid.c (mlx5_core_roce_gid_set)
 *        + drivers/net/ethernet/mellanox/mlx5/core/rdma.c (make_default_gid)
 * macOS difference: userspace policy supplies address changes to the driver.
 */
#include "MlxGID.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxGID, OSObject)

void MlxGID::free()
{
    stopAddressMonitor();
    if (fTable) {
        IOFree(fTable, sizeof(MlxGIDEntry) * fTableSize);
        fTable = NULL;
    }
    if (fUsed) {
        IOFree(fUsed, sizeof(bool) * fTableSize);
        fUsed = NULL;
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    super::free();
}

bool MlxGID::init(MlxRoCE *roce, uint32_t tableSize)
{
    if (!super::init())
        return false;

    fRoce = roce;
    fCore = roce->getCore();
    fTableSize = tableSize;
    fTable = (MlxGIDEntry *)IOMallocZero(sizeof(MlxGIDEntry) * tableSize);
    fUsed = (bool *)IOMallocZero(sizeof(bool) * tableSize);
    fLock = IOLockAlloc();
    fMonitoring = false;
    if (!fTable || !fUsed || !fLock)
        return false;
    return true;
}

uint32_t MlxGID::allocGIDIndex()
{
    uint32_t index = 0xFFFFFFFF;
    IOLockLock(fLock);
    for (uint32_t i = 0; i < fTableSize; i++) {
        if (!fUsed[i]) {
            fUsed[i] = true;
            index = i;
            break;
        }
    }
    IOLockUnlock(fLock);
    return index;
}

void MlxGID::freeGIDIndex(uint32_t index)
{
    IOLockLock(fLock);
    if (index < fTableSize) {
        fUsed[index] = false;
        fTable[index].used = false;
    }
    IOLockUnlock(fLock);
}

kern_return_t MlxGID::cmdSetRoceAddr(uint32_t index, const uint8_t *gid,
                                     const uint8_t *mac, uint8_t version,
                                     uint8_t l3Type, bool vlanEn,
                                     uint16_t vlanId)
{
    /* See gid.c:117 mlx5_core_roce_gid_set
     * set_roce_address_in:
     *   64B header + roce_address_index(4) + vhca_port_num(4) + roce_address(256B) */
    uint8_t in[400] = {};
    uint32_t addrOff = 0x60;                    /* roce_address offset */
    uint32_t l3Off = addrOff + 0;               /* source_l3_address[16] */
    uint32_t macHiOff = addrOff + 0x90;         /* source_mac_47_32 (0x80+0x10) */
    uint32_t macLoOff = addrOff + 0x94;         /* source_mac_31_0 */
    uint32_t l3TypeOff = addrOff + 0xC0 + 0x14; /* roce_l3_type */
    uint32_t verOff = l3TypeOff + 4;            /* roce_version */

    OSWriteBigInt16(in, 0, MLX_CMD_OP_SET_ROCE_ADDRESS);
    OSWriteBigInt32(in, 0x40, index);           /* roce_address_index */
    in[0x54] = 1;                               /* vhca_port_num = 1 (low 4 bits) */

    if (gid) {
        /* source_l3_address: 16-byte GID */
        memcpy(in + l3Off, gid, 16);
        /* source_mac: 6-byte MAC */
        uint32_t macHi = (uint32_t)mac[0] << 16 | (uint32_t)mac[1] << 8 | mac[2];
        uint32_t macLo = (uint32_t)mac[3] << 24 | (uint32_t)mac[4] << 16 |
                         (uint32_t)mac[5] << 8;
        OSWriteBigInt32(in, macHiOff, macHi);
        OSWriteBigInt32(in, macLoOff, macLo);
        /* vlan */
        if (vlanEn) {
            /* vlan_valid at 0x80 bit3, vlan_id at 0x81 */
            in[addrOff + 0x80] |= 0x08;
            in[addrOff + 0x81] = (uint8_t)((vlanId >> 4) & 0xFF);
            in[addrOff + 0x82] = (uint8_t)((vlanId & 0xF) << 4);
        }
    }

    /* roce_l3_type (4bit) + roce_version (8bit) */
    in[l3TypeOff] = (uint8_t)(l3Type & 0xF);
    in[verOff] = version;

    uint8_t out[16] = {};
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_SET_ROCE_ADDRESS };
    return fCore->exec(MLX_CMD_OP_SET_ROCE_ADDRESS, in, sizeof(in),
                       out, sizeof(out), 5000);
}

kern_return_t MlxGID::setGID(uint32_t index, const uint8_t *gid,
                             const uint8_t *mac, uint8_t roceVersion,
                             uint8_t l3Type, bool vlanEn, uint16_t vlanId)
{
    kern_return_t kr = cmdSetRoceAddr(index, gid, mac, roceVersion,
                                      l3Type, vlanEn, vlanId);
    if (kr != kIOReturnSuccess)
        return kr;

    IOLockLock(fLock);
    if (index < fTableSize) {
        fTable[index].used = true;
        fTable[index].index = index;
        memcpy(fTable[index].gid, gid, 16);
        memcpy(fTable[index].mac, mac, 6);
        fTable[index].roceVersion = roceVersion;
        fTable[index].l3Type = l3Type;
        fTable[index].vlanId = vlanId;
        fTable[index].vlanEn = vlanEn;
    }
    IOLockUnlock(fLock);

    IOLog("MlxGID: GID[%u] written version=%u l3=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
          index, roceVersion, l3Type,
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return kIOReturnSuccess;
}

kern_return_t MlxGID::delGID(uint32_t index)
{
    /* Delete = write an empty GID (see rdma.c mlx5_rdma_del_roce_addr) */
    uint8_t zeroGid[16] = {};
    uint8_t zeroMac[6] = {};
    kern_return_t kr = cmdSetRoceAddr(index, zeroGid, zeroMac, 0, 0,
                                      false, 0);
    freeGIDIndex(index);
    return kr;
}

void MlxGID::startAddressMonitor()
{
    /* Address configuration is a userspace policy operation. A daemon can
     * update GIDs through a constrained control method when implemented. */
    fMonitoring = false;
}

void MlxGID::stopAddressMonitor()
{
    fMonitoring = false;
}

kern_return_t MlxGID::getLocalAddr(uint8_t *gid, uint8_t *mac)
{
    /* Default IPv6 link-local GID (see rdma.c:122 mlx5_rdma_make_default_gid)
     * fe80::/64 + EUI-48 (MAC with the U/L bit inverted) */
    memset(gid, 0, 16);
    gid[0] = 0xFE;
    gid[1] = 0x80;
    /* EUI-64: set the U/L bit of the MAC */
    uint8_t m[6] = {0x00, 0x02, 0xC9, 0x00, 0x00, 0x01};
    m[0] |= 0x02;
    gid[8] = m[0];
    gid[9] = m[1];
    gid[10] = m[2];
    gid[11] = 0xFF;
    gid[12] = 0xFE;
    gid[13] = m[3];
    gid[14] = m[4];
    gid[15] = m[5];
    memcpy(mac, m, 6);
    return kIOReturnSuccess;
}
