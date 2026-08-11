/*
 * MlxHCA.hpp — Hardware abstraction layer (generic Mellanox mlx5 family)
 *
 * Core decoupling point: all driver logic accesses hardware capabilities
 * through this interface without depending directly on a specific model.
 * First implementation: ConnectX-5 (mcx5); later models only need to fill in
 * PCI IDs + capability differences.
 *
 * See: the MLX5_CAP_GEN / MLX5_CAP_ROCE macro families in device.h
 */
#ifndef MLX_HCA_HPP
#define MLX_HCA_HPP

#include <stdint.h>
#include <libkern/OSTypes.h>
#include <mach/kern_return.h>
#include "MlxRegs.hpp"
#include "MlxUCIO.h"   /* MLX_LINK_LAYER_* constants */

class MlxPCIDriver;

/* Port types (firmware port_type values) */
enum MlxPortType {
    MLX_PORT_TYPE_IB  = 0,
    MLX_PORT_TYPE_ETH = 1,
};

/* IB address parameters (plan C: reserved for IB addressing, See ah.c:97-98) */
struct MlxIBAddr {
    uint16_t dlid;          /* destination LID */
    uint8_t  pathBits;      /* path bits */
    uint8_t  sl;            /* service level (8-bit IB vs 3-bit RoCE) */
};

/*
 * Hardware capability summary — key fields returned by QUERY_HCA_CAP
 * Different models fill it via MLX_GET(MLX5_CMD_OP_QUERY_HCA_CAP, ...)
 */
struct MlxHcaCaps {
    /* generic */
    uint32_t fwRev;
    uint16_t cmdifRev;
    uint8_t  portType;          /* See MlxPortType: 0=IB 1=Ethernet */
    uint32_t numPorts;

    /* resource limits */
    uint32_t maxQp;
    uint32_t maxCq;
    uint32_t maxMr;

    /* RoCE (See mlx5_ifc.h:1140 roce_cap_bits) */
    bool     roce;              /* device supports RoCE */
    bool     roceRwSupported;   /* RoCE switch is writable */
    uint8_t  roceVersions;      /* bit0=RoCEv1, bit1=RoCEv2 support bits */
    uint16_t roceMaxGid;        /* GID table size */
    uint16_t roceDstUdpPort;    /* RoCEv2 destination port (4791) */
    uint16_t roceMinSrcUdpPort; /* source port lower bound */
    bool     roceCcCaps;        /* congestion control capability */
    bool     dcqcnEnabled;      /* DCQCN enabled */

    /* hardware features */
    bool     uar4k;             /* 4K UAR support */
    uint8_t  logBfRegSize;      /* log2 bytes per BF register */
    bool     swRoceSrcUdpPort;  /* RoCEv2 source UDP port settable */

    /* IB capabilities (reserved for plan C) */
    uint16_t ibMaxLids;         /* LID table size */
    uint16_t ibMaxPkeys;        /* P_Key table size */
    bool     ibSupported;       /* IB mode supported */

    /* link-layer helpers */
    bool isEthernet() const { return portType == MLX_PORT_TYPE_ETH; }
    bool isIB() const { return portType == MLX_PORT_TYPE_IB; }
    int linkLayer() const {
        return isEthernet() ? MLX_LINK_LAYER_ETHERNET :
               isIB() ? MLX_LINK_LAYER_INFINIBAND :
               MLX_LINK_LAYER_UNSPECIFIED;
    }
};

/*
 * Vendor information
 */
struct MlxVendorInfo {
    uint16_t vendorId;
    uint16_t deviceId;
    uint32_t revision;
};

/*
 * HCA abstract interface — all sub-modules (QP/CQ/MR/AH) depend only on this interface
 */
class MlxHCA {
public:
    virtual ~MlxHCA() {}
    virtual void attachCore(MlxPCIDriver *core) = 0;

    /* Capabilities */
    virtual const MlxHcaCaps &caps() const = 0;
    virtual const MlxVendorInfo &vendor() const = 0;
    virtual MlxHcaCaps &mutableCaps() = 0;
    virtual MlxVendorInfo &mutableVendor() = 0;

    /* Command execution (implemented by MlxCmd) */
    virtual kern_return_t exec(uint32_t opcode, const void *in,
                               uint32_t inSize, void *out,
                               uint32_t outSize, uint32_t timeoutMs) = 0;

    /* Register access */
    virtual uint32_t readReg(void *mmioOffset) = 0;
    virtual void writeReg(void *mmioOffset, uint32_t value) = 0;

    /* UAR mapping (for data path/user space) */
    virtual void *getUarVirtual() = 0;
    virtual uint64_t getUarPhysical() = 0;
};

/*
 * Hardware capability loader — handles model dispatch
 * Concrete implementations: MlxHCAConnectX5 (first, See MlxHCAConnectX5.cpp),
 * then ConnectX6/7...
 */
class MlxHCALoader {
public:
    /* Select the HCA implementation matching the PCI ID */
    static MlxHCA *create(uint16_t deviceId);

    /* Per-model sub-factories (called internally by create, spread across each model's .cpp) */
    static MlxHCA *createCx4(uint16_t deviceId);   /* MlxHCAConnectX4.cpp */
    static MlxHCA *createCx5(uint16_t deviceId);   /* MlxHCAConnectX5.cpp */
    static MlxHCA *createCx6(uint16_t deviceId);   /* MlxHCAConnectX6.cpp */
    static MlxHCA *createCx7(uint16_t deviceId);   /* MlxHCAConnectX7.cpp */
};

#endif /* MLX_HCA_HPP */
