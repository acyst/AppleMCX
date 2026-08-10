/*
 * MlxDMA.hpp — DMA mapping utility (generic Mellanox family)
 *
 * Responsibility: pinning user memory to physical (DMA) addresses and lookup
 *
 * RDMA DMA integration core: hardware DMA needs physical addresses while
 * user space only has virtual addresses. This class pins user buffers with
 * IOMemoryDescriptor::withAddressRange and obtains the physical segments.
 */
#ifndef MLX_DMA_HPP
#define MLX_DMA_HPP

#include <libkern/OSTypes.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include "MlxRegs.hpp"

#define MLX_DMA_MAX_SEGS     64

/*
 * DMA mapping result: physical segment list of user memory
 */
struct MlxDMAReq {
    task_t    ownerTask;        /* task that owns the virtual range */
    uint64_t  virtAddr;         /* user virtual address */
    uint64_t  length;           /* length */
    uint64_t  paList[MLX_DMA_MAX_SEGS];   /* physical segment list */
    uint64_t  paLenList[MLX_DMA_MAX_SEGS];/* physical length of each segment */
    uint32_t  numSegs;
    IOMemoryDescriptor *memDesc;          /* pinned memory descriptor */
    IODMACommand *dmaCommand;             /* retained IOMMU mapping */
};

/*
 * DMA mapping utility
 */
class MlxDMA : public OSObject {
    OSDeclareDefaultStructors(MlxDMA)

public:
    bool init();
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* pin user memory → physical segment list
     * See MlxMR::buildPBL (mr.c) + ib_umem_get */
    kern_return_t pinUserMemory(uint64_t virtAddr, uint64_t length,
                                MlxDMAReq *req);

    /* Release the pin */
    void unpinMemory(MlxDMAReq *req);

    /* Map physical address → user address lookup (for post_send) */
    uint64_t lookupPhys(task_t ownerTask, uint64_t virtAddr);

private:
    /* Get physical segments (IODMACommand, handles multiple segments) */
    kern_return_t getSegments(IOMemoryDescriptor *mem,
                              uint64_t *paList, uint32_t *numSegs);

    OSArray *fPendingReqs;      /* active pin records */
    IOLock  *fLock;
};

#endif /* MLX_DMA_HPP */
