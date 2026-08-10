/*
 * MlxCC.cpp — DCQCN congestion control implementation (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/cong.c + core/port.c (mlx5_set_roce_cc_param)
 * Key point: the DCQCN control loop runs in firmware; the driver only wraps the QUERY/MODIFY_CONG_PARAMS commands
 */
#include "MlxCC.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxCC, OSObject)

void MlxCC::free()
{
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    super::free();
}

bool MlxCC::init(MlxRoCE *roce)
{
    if (!super::init())
        return false;
    fRoce = roce;
    fCore = roce->getCore();
    fLock = IOLockAlloc();
    fEnabled = false;

    /* Default parameters (see cong.c defaults) */
    memset(&fParams, 0, sizeof(fParams));
    fParams.rpgMinDecFac = 256;      /* 1/256 */
    fParams.rpgAiRate = 5;           /* 5 Mbps */
    fParams.rpgTimeReset = 55;       /* 55 us */
    fParams.rpgThreshold = 150;      /* 150KB */
    return fLock != NULL;
}

kern_return_t MlxCC::cmdQuery(uint32_t regId, void *out)
{
    (void)regId;
    (void)out;
    return kIOReturnUnsupported;
}

kern_return_t MlxCC::cmdModify(uint32_t regId, const void *in)
{
    (void)regId;
    (void)in;
    return kIOReturnUnsupported;
}

kern_return_t MlxCC::queryParams(struct mlx_cc_params *out)
{
    if (!out)
        return kIOReturnBadArgument;
    IOLockLock(fLock);
    memcpy(out, &fParams, sizeof(struct mlx_cc_params));
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

kern_return_t MlxCC::modifyParams(const struct mlx_cc_params *in)
{
    if (!in)
        return kIOReturnBadArgument;

    /* See core/port.c:900 mlx5_set_roce_cc_param
     * MODIFY_CONG_PARAMS command (opcode 0x825) */
    uint8_t buf[512] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(buf, 0, MLX_CMD_OP_MODIFY_CONG_PARAMS);

    /* cong_control_r_roce_ecn_rp/np parameter area (see mlx5_ifc.h:2255) */
    /* floor fields: rpg_min_dec_fac, rpg_ai_rate, rpg_time_reset ... */
    OSWriteBigInt32(buf, 0x40, in->rpgMinDecFac);
    OSWriteBigInt32(buf, 0x48, in->rpgAiRate);
    OSWriteBigInt32(buf, 0x50, in->rpgTimeReset);
    OSWriteBigInt32(buf, 0x58, in->rpgThreshold);

    kern_return_t kr = fCore->exec(MLX_CMD_OP_MODIFY_CONG_PARAMS,
                                   buf, sizeof(buf), out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) {
        IOLockLock(fLock);
        memcpy(&fParams, in, sizeof(struct mlx_cc_params));
        fEnabled = true;
        IOLockUnlock(fLock);
        IOLog("MlxCC: DCQCN parameters updated (min_dec_fac=%u ai_rate=%u)\n",
              in->rpgMinDecFac, in->rpgAiRate);
    }
    return kr;
}
