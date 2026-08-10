/*
 * MlxMR.hpp — Memory Registration (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/mr.c (trimmed: reg_user_mr/dereg, no ODP)
 * Pins user memory via IOMemoryDescriptor, builds the PBL, and issues CREATE_MKEY
 */
#ifndef MLX_MR_HPP
#define MLX_MR_HPP

#include <libkern/OSTypes.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include "MlxUCIO.h"

class MlxRoCE;
class MlxPCIDriver;

#define MLX_MAX_MR_SEGMENTS     64

/*
 * MR instance (see struct mlx5_ib_mr)
 */
struct MlxMRContext {
    uint32_t    mrHandle;
    uint32_t    pd;
    uint32_t    lkey;
    uint32_t    rkey;
    uint64_t    startAddr;
    uint64_t    length;
    uint32_t    accessFlags;
    IOMemoryDescriptor *fMemDesc;   /* pinned user memory */
    IODMACommand *fDmaCommand;      /* retained IOMMU mapping */
};

/*
 * MR management class
 */
class MlxMR : public OSObject {
    OSDeclareDefaultStructors(MlxMR)

public:
    bool init(MlxRoCE *roce);
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* Register a user MR (see mlx5_ib_reg_user_mr, mr.c:1391) */
    kern_return_t regMR(const struct mlx_reg_mr_req *req,
                        struct mlx_reg_mr_resp *resp);
    kern_return_t deregMR(uint32_t mrHandle);
    MlxMRContext *lookup(uint32_t mrHandle);

private:
    /* Build the PBL (physical address list); see reg_create, mr.c:1097 */
    kern_return_t buildPBL(IOMemoryDescriptor *mem, uint64_t *paList,
                           uint32_t *numSegs, IODMACommand **dmaCommand);

    /* CREATE_MKEY command (see mlx5_ib_create_mkey, mr.c:101) */
    kern_return_t cmdCreateMKey(const uint64_t *paList, uint32_t numSegs,
                                uint64_t startAddr, uint64_t length,
                                uint32_t accessFlags, uint32_t pd,
                                uint32_t *mkey, uint32_t *lkey,
                                uint32_t *rkey);
    kern_return_t cmdDestroyMKey(uint32_t mkey);

    MlxRoCE     *fRoce;
    MlxPCIDriver *fCore;
    OSArray     *fMrTable;
    IOLock      *fLock;
};

#endif /* MLX_MR_HPP */
