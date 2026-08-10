/*
 * MlxUAR.hpp — User Access Region (UAR) management (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/net/ethernet/mellanox/mlx5/core/uar.c
 * A UAR is a 4K page mapped into the system address space containing
 * doorbell/BF registers; after user-space mmap the doorbell is written
 * directly with zero syscalls.
 */
#ifndef MLX_UAR_HPP
#define MLX_UAR_HPP

#include <libkern/OSTypes.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include "MlxRegs.hpp"

class MlxPCIDriver;

/*
 * UAR allocation result
 */
struct MlxUarAlloc {
    uint32_t    index;          /* UAR index */
    void       *map;            /* mapped virtual address (within BAR0) */
    uint64_t    phys;           /* physical address */
    bool        wc;             /* write-combining */
};

/*
 * BF register allocation result
 */
struct MlxBfreg {
    void       *map;            /* BF register virtual address (uar + offset) */
    uint32_t    offset;         /* offset relative to the UAR (includes MLX_BF_OFFSET) */
};

/*
 * UAR/BF management class
 */
class MlxUAR : public OSObject {
    OSDeclareDefaultStructors(MlxUAR)

public:
    bool init(MlxPCIDriver *owner);
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* Allocate a UAR page (See mlx5_get_uars_page, uar.c:165) */
    kern_return_t allocUar(MlxUarAlloc *uar);
    void freeUar(MlxUarAlloc *uar);

    /* Allocate a BF register (See mlx5_alloc_bfreg, uar.c:258) */
    kern_return_t allocBfreg(MlxBfreg *bfreg);
    void freeBfreg(MlxBfreg *bfreg);

    /* For user-space mapping */
    IOMemoryDescriptor *getUarMemDesc() { return fUarMemDesc; }
    IOMemoryMap *getUarMap() { return fUarMap; }

    /* boot UAR index (used for EQ/CQ uar_page, See priv->uar->index) */
    uint32_t getBootUarIndex() const { return fBootIndex; }

    /* DB record page (user-space post_send updates the SQ/RQ head pointers)
     * See mlx5_db_alloc, driver.h:1006 struct mlx5_db */
    kern_return_t allocDbRecord();
    void freeDbRecord();
    uint32_t *getDbRecord() const { return fDbRecord; }
    uint64_t getDbRecordDMA() const { return fDbRecordDMA; }
    IOMemoryDescriptor *getDbMemDesc() { return fDbMemDesc; }
    kern_return_t allocDbSlots(uint32_t count, uint32_t *offset);
    void freeDbSlots(uint32_t offset, uint32_t count);

private:
    /* Issue the ALLOC_UAR command (See uar.c:38 mlx5_cmd_alloc_uar) */
    kern_return_t cmdAllocUar(uint32_t *index);
    kern_return_t cmdFreeUar(uint32_t index);

    /* Compute the UAR physical address (See uar.c:70 uar2pfn) */
    uint64_t uarPhys(uint32_t index);

    MlxPCIDriver *fOwner;
    OSArray     *fUarPool;       /* pool of allocated UARs */
    IOMemoryDescriptor *fUarMemDesc;
    IOMemoryMap        *fUarMap;
    uint32_t    fUarBitmap;      /* BF allocation bitmap */
    uint32_t    fBootIndex;      /* boot UAR index */
    MlxUarAlloc fBootUar;
    bool        fBootUarValid;
    uint32_t   *fDbRecord;       /* DB record page */
    uint64_t    fDbRecordDMA;
    IOBufferMemoryDescriptor *fDbMemDesc;
    IODMACommand *fDbDmaMap;
    uint64_t    fDbBitmap[2];
    IOLock     *fDbLock;
};

#endif /* MLX_UAR_HPP */
