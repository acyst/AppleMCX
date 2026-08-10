/*
 * MlxAH.hpp — Address Handle (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/ah.c (complete)
 * Encodes the destination IP/GID/MAC into the hardware mlx5_av
 */
#ifndef MLX_AH_HPP
#define MLX_AH_HPP

#include <libkern/OSTypes.h>
#include <libkern/c++/OSContainers.h>
#include <mach/kern_return.h>
#include "MlxWQE.hpp"
#include "MlxUCIO.h"

class MlxRoCE;
class MlxPCIDriver;

/*
 * AH instance (see struct mlx5_ib_ah)
 */
struct MlxAHContext {
    uint32_t    ahHandle;
    uint32_t    portNum;
    MlxAV       av;             /* encoded address vector */
    bool        isRoCE;
};

/*
 * AH management class
 */
class MlxAH : public OSObject {
    OSDeclareDefaultStructors(MlxAH)

public:
    bool init(MlxRoCE *roce);
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* Create an AH: encode mlx5_av (see create_ib_ah, ah.c:53) */
    kern_return_t createAH(const struct mlx_create_ah_req *req,
                           struct mlx_create_ah_resp *resp);
    kern_return_t destroyAH(uint32_t ahHandle);
    MlxAHContext *lookup(uint32_t ahHandle);

    /* Encoding core (see ah.c:59-95 field by field) */
    static void encodeAV(const struct mlx_create_ah_req *req, MlxAV *av);

private:
    MlxRoCE *fRoce;
    OSArray *fAhTable;
    IOLock  *fLock;
    uint32_t fNextHandle;
};

#endif /* MLX_AH_HPP */
