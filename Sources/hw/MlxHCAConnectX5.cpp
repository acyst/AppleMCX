/*
 * MlxHCAConnectX5.cpp — ConnectX-5 hardware implementation (first model)
 *
 * Framework decoupling: a concrete implementation of the MlxHCA abstract
 * interface, dispatched via MlxHCALoader::create(deviceId).
 * Adding a model (ConnectX-6/7...) = new implementation class + capability deltas.
 *
 * Ported from: mlx5_core/main.c (handle_hca_cap) + port.c (capability query)
 */
#include "MlxHCA.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"
#include "MlxUAR.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

/* ---- ConnectX-5 concrete implementation ---- */

class MlxHCAConnectX5 : public MlxHCA {
public:
    MlxHCAConnectX5() : fCore(NULL), fLoaded(false), fCaps{}, fVendor{} {}

    virtual ~MlxHCAConnectX5() {}

    /* Bind the core driver (the loader only dispatches; core is set in the start phase) */
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
        /* MVP: UAR virtual address is provided in phase P1 (after ioremap) */
        return NULL;
    }

    virtual uint64_t getUarPhysical() APPLE_KEXT_OVERRIDE { return 0; }

    /* ConnectX-5 capability loading */
    kern_return_t loadCaps();

private:
    MlxPCIDriver  *fCore;
    bool           fLoaded;
    MlxHcaCaps     fCaps;
    MlxVendorInfo  fVendor;
};

/* ---- Factory: model dispatch ---- */

MlxHCA *MlxHCALoader::create(uint16_t deviceId)
{
    /* Unified dispatch: try each sub-factory by generation
     * Future: add createCx8 and so on */
    MlxHCA *hca = MlxHCALoader::createCx4(deviceId);
    if (hca)
        return hca;
    hca = MlxHCALoader::createCx5(deviceId);
    if (hca)
        return hca;
    hca = MlxHCALoader::createCx6(deviceId);
    if (hca)
        return hca;
    hca = MlxHCALoader::createCx7(deviceId);
    if (hca)
        return hca;
    return NULL;
}

MlxHCA *MlxHCALoader::createCx5(uint16_t deviceId)
{
    /* ConnectX-5 family: 0x1017 (PF), 0x1019 (Ex) */
    switch (deviceId) {
    case 0x1017:
    case 0x1018:    /* VF */
    case 0x1019:    /* Ex */
    case 0x101A:    /* Ex VF */
        return new MlxHCAConnectX5;
    default:
        return NULL;
    }
}
