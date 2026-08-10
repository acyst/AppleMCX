/*
 * MlxHCAConnectX4.cpp — ConnectX-4 hardware implementation
 *
 * Key facts (verified from mlx5_core): the driver shares one code base for all
 * ConnectX models, with no per-model branches. Model differences appear only in:
 *   1. PCI IDs (ConnectX-4 PF 0x1013, VF 0x1014; 4LX PF 0x1015, VF 0x1016)
 *   2. Firmware version / capability registers (QUERY_HCA_CAP returns different
 *      values; the driver adapts automatically)
 *   3. ISSI negotiation (older ConnectX-4 firmware may only support ISSI=0,
 *      already handled by the setIssi fallback)
 *
 * Therefore ConnectX-4 shares most of its logic with ConnectX-5; this class only
 * annotates the model differences.
 */
#include "MlxHCA.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

/* ---- ConnectX-4 concrete implementation ---- */

class MlxHCAConnectX4 : public MlxHCA {
public:
    MlxHCAConnectX4() : fCore(NULL), fLoaded(false), fCaps{}, fVendor{} {}

    virtual ~MlxHCAConnectX4() {}

    virtual void attachCore(MlxPCIDriver *core) APPLE_KEXT_OVERRIDE
    { fCore = core; }

    virtual const MlxHcaCaps &caps() const APPLE_KEXT_OVERRIDE { return fCaps; }
    virtual const MlxVendorInfo &vendor() const APPLE_KEXT_OVERRIDE { return fVendor; }
    virtual MlxHcaCaps &mutableCaps() APPLE_KEXT_OVERRIDE { return fCaps; }
    virtual MlxVendorInfo &mutableVendor() APPLE_KEXT_OVERRIDE { return fVendor; }

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

    /* ConnectX-4 capability loading (same as ConnectX-5; firmware returns actual values) */
    kern_return_t loadCaps();

private:
    MlxPCIDriver  *fCore;
    bool           fLoaded;
    MlxHcaCaps     fCaps;
    MlxVendorInfo  fVendor;
};

/* ---- Factory dispatch (ConnectX-4 family) ---- */

MlxHCA *MlxHCALoader::createCx4(uint16_t deviceId)
{
    /* ConnectX-4 PF 0x1013, VF 0x1014; ConnectX-4LX PF 0x1015, VF 0x1016 */
    switch (deviceId) {
    case 0x1013:    /* ConnectX-4 */
    case 0x1014:    /* ConnectX-4 VF */
    case 0x1015:    /* ConnectX-4LX */
    case 0x1016:    /* ConnectX-4LX VF */
        return new MlxHCAConnectX4;
    default:
        return NULL;
    }
}
