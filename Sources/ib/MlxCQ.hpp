/*
 * MlxCQ.hpp — Completion Queue (generic Mellanox family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/infiniband/hw/mlx5/cq.c
 * Trimmed: create/destroy; poll_cq reads the CQE buffer directly from userspace (zero-copy)
 */
#ifndef MLX_CQ_HPP
#define MLX_CQ_HPP

#include <libkern/OSTypes.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "MlxWQE.hpp"
#include "MlxEQ.hpp"

class MlxRoCE;

/*
 * CQ context
 */
struct MlxCQContext {
    uint32_t    cqNumber;
    uint32_t    logSize;        /* log of depth */
    uint32_t    cqeSize;        /* 64 bytes */
    /* CQE buffer (kernel DMA allocation) */
    uint64_t    cqeBufAddr;     /* virtual address */
    uint64_t    cqeDMA;         /* physical address */
    IOBufferMemoryDescriptor *cqeBufDesc;  /* buffer descriptor (held) */
    IODMACommand *cqeDmaMap;               /* retained IOMMU mapping */
    uint64_t    pageDMA[32];
    uint32_t    numPages;
    uint32_t    dbRecordOffset;
    uint32_t    eqNumber;
    uint32_t    compVector;
    uint32_t    armSn;          /* arm sequence number */
    /* completion callback (see ib_cq->comp_handler, cq.c:41) */
    void (*completionHandler)(uint32_t cqn, void *context);
    void       *completionContext;
    uint64_t    completions;    /* completion count */
};

/*
 * CQ completion event handler (see mlx5_ib_cq_comp, cq.c:41)
 * MlxRoCE subscribes to EQ COMPLETION events and dispatches by cqn to the corresponding CQ
 */
class MlxCQ : public OSObject {
    OSDeclareDefaultStructors(MlxCQ)

public:
    bool init(MlxRoCE *roce);
    virtual void free() APPLE_KEXT_OVERRIDE;

    kern_return_t createCQ(uint32_t entries,
                           struct mlx_create_cq_resp *resp);
    kern_return_t destroyCQ(uint32_t cqHandle);

    MlxCQContext *lookup(uint32_t cqHandle);

    /* Get the CQ buffer descriptor (for userspace poll_cq mapping) */
    IOMemoryDescriptor *getCqMemDesc(uint32_t cqHandle);

    /* Completion event dispatch: called by MlxRoCE::handleEvent
     * See eq.c:106 mlx5_eq_comp_int: look up the CQ by cqn → call the completion callback */
    void handleCompletion(uint32_t cqn);

    /* Query the CQ completion count (used by userspace ibv_get_cq_event) */
    uint64_t getCompletions(uint32_t cqHandle);

private:
    /* CREATE_CQ command (see create_cq_user, cq.c:717) */
    kern_return_t cmdCreateCQ(MlxCQContext *cq, uint32_t eqNumber);
    kern_return_t cmdDestroyCQ(uint32_t cqNumber);

    MlxRoCE *fRoce;
    OSArray *fCqTable;
    IOLock  *fLock;
};

#endif /* MLX_CQ_HPP */
