/*
 * MlxHCAConnectX6.cpp — ConnectX-6 hardware implementation
 *
 * Key facts (verified from mlx5_core): shares the same driver code as
 * ConnectX-5, with no per-model branches.
 * ConnectX-6 features (hardware capabilities, auto-adapted by firmware):
 *   - 200GbE support
 *   - Full DCQCN congestion control (roce_cc_caps)
 *   - Enhanced QoS
 * PCI IDs: 0x101B (CX6), 0x101D (CX6 Dx), 0x101F (CX6 LX)
 */
#include "MlxHCA.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

/* ---- ConnectX-6 concrete implementation ---- */

class MlxHCAConnectX6 : public MlxHCA {
public:
    MlxHCAConnectX6() : fCore(NULL), fLoaded(false) {}

    virtual ~MlxHCAConnectX6() {}

    void attachCore(MlxPCIDriver *core) { fCore = core; }

    virtual const MlxHcaCaps &caps() const APPLE_KEXT_OVERRIDE { return fCaps; }
    virtual const MlxVendorInfo &vendor() const APPLE_KEXT_OVERRIDE { return fVendor; }

    virtual kern_return_t exec(uint32_t opcode, const void *in,
                               uint32_t inSize, void *out,
                               uint32_t outSize, uint32_t timeoutMs) APPLE_KEXT_OVERRIDE
    {
        return fCore->exec(opcode, in, inSize, out, outSize, timeoutMs);
    }

    virtual uint32_t readReg(void *mmioOffset) APPLE_KEXT_OVERRIDE
    {
        return IORead32(fCore->getBar0(), (uintptr_t)mmioOffset);
    }

    virtual void writeReg(void *mmioOffset, uint32_t value) APPLE_KEXT_OVERRIDE
    {
        IOWrite32(fCore->getBar0(), (uintptr_t)mmioOffset, value);
    }

    virtual void *getUarVirtual() APPLE_KEXT_OVERRIDE
    {
        return NULL;
    }

    virtual uint64_t getUarPhysical() APPLE_KEXT_OVERRIDE { return 0; }

    /* ConnectX-6 capability loading (firmware returns actual values, incl. DCQCN/QoS capabilities) */
    kern_return_t loadCaps();

private:
    MlxPCIDriver  *fCore;
    bool           fLoaded;
    MlxHcaCaps     fCaps;
    MlxVendorInfo  fVendor;
};

/* ---- Factory dispatch (ConnectX-6 family) ---- */

MlxHCA *MlxHCALoader::createCx6(uint16_t deviceId)
{
    /* ConnectX-6 PF 0x101B, VF 0x101C
     * ConnectX-6 Dx PF 0x101D, VF 0x101E
     * ConnectX-6 LX PF 0x101F */
    switch (deviceId) {
    case 0x101B:    /* ConnectX-6 */
    case 0x101C:    /* ConnectX-6 VF */
    case 0x101D:    /* ConnectX-6 Dx */
    case 0x101E:    /* ConnectX-6 Dx VF */
    case 0x101F:    /* ConnectX-6 LX */
        return new MlxHCAConnectX6;
    default:
        return NULL;
    }
}

/* Exported bind function (used by MlxPCIDriver) */
extern "C" void
mlx_hca_attach_core_cx6(MlxHCA *hca, MlxPCIDriver *core)
{
    MlxHCAConnectX6 *cx6 = dynamic_cast<MlxHCAConnectX6 *>(hca);
    if (cx6)
        cx6->attachCore(core);
}
