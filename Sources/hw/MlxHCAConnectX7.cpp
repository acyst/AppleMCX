/*
 * MlxHCAConnectX7.cpp — ConnectX-7 / ConnectX-8 hardware implementation
 *
 * Key facts (verified from mlx5_core): shares the same driver code as CX5/CX6,
 * with no per-model branches.
 *   - ConnectX-7: 400GbE NDR (PCI ID 0x1021, VF 0x1022)
 *   - ConnectX-8: higher bandwidth (PCI ID 0x1023)
 *   - BlueField-3 with integrated CX7: 0xa2dc
 *   - BlueField-4 with integrated CX8: 0xa2df
 * All features are auto-adapted via the firmware capability registers.
 */
#include "MlxHCA.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

/* ---- ConnectX-7/8 concrete implementation (shared) ---- */

class MlxHCAConnectX7 : public MlxHCA {
public:
    MlxHCAConnectX7() : fCore(NULL), fLoaded(false) {}

    virtual ~MlxHCAConnectX7() {}

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
        return mlxMMIORead32BE(fCore->getBar0(), (uintptr_t)mmioOffset);
    }

    virtual void writeReg(void *mmioOffset, uint32_t value) APPLE_KEXT_OVERRIDE
    {
        mlxMMIOWrite32BE(fCore->getBar0(), (uintptr_t)mmioOffset, value);
    }

    virtual void *getUarVirtual() APPLE_KEXT_OVERRIDE
    {
        return NULL;
    }

    virtual uint64_t getUarPhysical() APPLE_KEXT_OVERRIDE { return 0; }

    kern_return_t loadCaps();

private:
    MlxPCIDriver  *fCore;
    bool           fLoaded;
    MlxHcaCaps     fCaps;
    MlxVendorInfo  fVendor;
};

/* ---- Factory dispatch (ConnectX-7/8 family) ---- */

MlxHCA *MlxHCALoader::createCx7(uint16_t deviceId)
{
    /* ConnectX-7 PF 0x1021, VF 0x1022
     * ConnectX-8 PF 0x1023
     * BlueField-3 with integrated CX7 0xa2dc
     * BlueField-4 with integrated CX8 0xa2df */
    switch (deviceId) {
    case 0x1021:    /* ConnectX-7 */
    case 0x1022:    /* ConnectX-7 VF */
    case 0x1023:    /* ConnectX-8 */
    case 0xa2dc:    /* BlueField-3 (CX7) */
    case 0xa2df:    /* BlueField-4 (CX8) */
        return new MlxHCAConnectX7;
    default:
        return NULL;
    }
}

/* Exported bind function (used by MlxPCIDriver) */
extern "C" void
mlx_hca_attach_core_cx7(MlxHCA *hca, MlxPCIDriver *core)
{
    MlxHCAConnectX7 *cx7 = dynamic_cast<MlxHCAConnectX7 *>(hca);
    if (cx7)
        cx7->attachCore(core);
}
