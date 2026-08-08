/*
 * MlxCC.hpp — DCQCN congestion control (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/cong.c + core/port.c:900 (mlx5_set_roce_cc_param)
 * Key point: the DCQCN control loop runs in firmware; the driver only wraps the QUERY/MODIFY_CONG_PARAMS commands
 */
#ifndef MLX_CC_HPP
#define MLX_CC_HPP

#include <libkern/OSTypes.h>
#include "MlxRegs.hpp"
#include "MlxUCIO.h"

class MlxRoCE;
class MlxPCIDriver;

/*
 * DCQCN congestion control management class
 */
class MlxCC : public OSObject {
    OSDeclareDefaultStructors(MlxCC)

public:
    bool init(MlxRoCE *roce);

    /* See core/port.c:900 mlx5_set_roce_cc_param */
    kern_return_t queryParams(struct mlx_cc_params *out);
    kern_return_t modifyParams(const struct mlx_cc_params *in);

    /* congestion control capability */
    bool isEnabled() { return fEnabled; }

private:
    /* QUERY_CONG_PARAMS / MODIFY_CONG_PARAMS commands */
    kern_return_t cmdQuery(uint32_t regId, void *out);
    kern_return_t cmdModify(uint32_t regId, const void *in);

    MlxRoCE      *fRoce;
    MlxPCIDriver *fCore;
    bool          fEnabled;
    struct mlx_cc_params fParams;
    IOLock       *fLock;
};

#endif /* MLX_CC_HPP */
