/*
 * MlxMain.cpp — KEXT main entry
 *
 * Defines the IOService metaclass registration (MlxPCIDriver) and module load/unload.
 */
#include "MlxPCIDriver.hpp"
#include "MlxRoCE.hpp"

#include <libkern/c++/OSKext.h>

/* IOService metaclass registration macros (required for every class derived from IOService) */
OSMetaClassDefineReservedUsed(MlxPCIDriver, 0)
OSMetaClassDefineReservedUsed(MlxPCIDriver, 1)

/* Module load entry */
extern "C" OSKextLoadResult
AppleMCX_start(struct OSKext *kext)
{
    (void)kext;
    IOLog("AppleMCX: driver loaded (generic Mellanox mlx5 family, first implementation ConnectX-5)\n");
    return kOSKextLoadSuccess;
}

/* Module unload entry */
extern "C" void
AppleMCX_stop(struct OSKext *kext)
{
    (void)kext;
    IOLog("AppleMCX: driver unloaded\n");
}
