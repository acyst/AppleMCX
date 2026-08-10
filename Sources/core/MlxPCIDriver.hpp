/*
 * MlxPCIDriver.hpp — PCI binding layer (core layer entry, generic Mellanox family)
 *
 * Responsibilities (See mlx5_core/main.c: probe_one + mlx5_init_one):
 *   1. PCI enumeration: BAR0 mapping, bus master enable
 *   2. Firmware initialization: ENABLE_HCA → SET_ISSI → SET_HCA_CAP → INIT_HCA
 *   3. Set up EQ / UAR / command interface
 *   4. Publish child-service nubs (mlx_rdma, mlx_eth) → trigger verbs/Ethernet layers
 */
#ifndef MLX_PCI_DRIVER_HPP
#define MLX_PCI_DRIVER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "MlxHCA.hpp"
#include "MlxCmd.hpp"
#include "MlxEQ.hpp"
#include "MlxUAR.hpp"
#include "MlxHealth.hpp"
#include "MlxDMA.hpp"

class MlxRoCE;
class MlxEthernetDriver;

/*
 * Core driver class — one instance per PCI device
 */
class MlxPCIDriver : public IOService {
    OSDeclareDefaultStructors(MlxPCIDriver)

public:
    /* IOService lifecycle */
    virtual bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* ---- Hardware access (for sub-modules) ---- */
    IOMemoryMap *getBar0() { return fBar0Map; }
    MlxCmd *getCmd() { return fCmd; }
    MlxEQ *getEQ() { return fEQ; }
    MlxUAR *getUAR() { return fUAR; }
    MlxHCA *getHCA() { return fHCA; }
    IOPCIDevice *getPCI() { return fPci; }
    MlxHealth *getHealth() { return fHealth; }
    MlxDMA *getDMA() { return fDMA; }

    /* Firmware command shortcut entry (implemented by MlxHCA::exec) */
    kern_return_t exec(uint32_t opcode, const void *in, uint32_t inSize,
                       void *out, uint32_t outSize, uint32_t timeoutMs);

    /* ISSI version (filled after negotiation in setIssi, 0 or 1) */
    uint32_t getIssi() const { return fIssi; }

    /* Device index (mlx5_0/mlx5_1/... for multiple devices) */
    uint32_t getDevIdx() const { return fDevIdx; }
    const char *getDevName() const { return fDevName; }

private:
    /* Firmware initialization sequence (See mlx5_function_setup, main.c:1361) */
    bool fwInit();
    bool enableHca();
    bool setIssi();
    bool setHcaCaps();
    bool initHca();
    bool teardownHca();
    bool disableHca();
    bool queryHcaCaps();
    void cleanup();

    /* Capability negotiation (See handle_hca_cap, main.c:712) */
    bool negotiateRoceCap();

    /* Publish child services */
    bool publishNubs();

    IOPCIDevice     *fPci;
    IOMemoryMap     *fBar0Map;
    MlxCmd          *fCmd;
    MlxEQ           *fEQ;
    MlxUAR          *fUAR;
    MlxHCA          *fHCA;
    MlxHealth       *fHealth;
    MlxDMA          *fDMA;
    MlxRoCE         *fRoCE;
    MlxEthernetDriver *fEth;
    uint16_t         fDeviceId;
    uint32_t         fIssi;       /* ISSI version (0 or 1), filled after setIssi */
    uint32_t         fDevIdx;     /* device index (multiple devices) */
    char             fDevName[16];/* device name mlx5_N */
};

#endif /* MLX_PCI_DRIVER_HPP */
