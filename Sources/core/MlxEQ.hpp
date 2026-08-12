/*
 * MlxEQ.hpp — Event queues (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/net/ethernet/mellanox/mlx5/core/eq.c
 * macOS differences: use IOInterruptEventSource + IOWorkLoop instead of Linux tasklet/notifier
 */
#ifndef MLX_EQ_HPP
#define MLX_EQ_HPP

#include <libkern/OSTypes.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include "MlxRegs.hpp"

#define MLX_EVENT_TYPE_MAX      256
#define MLX_NUM_ASYNC_EQS       3       /* cmd/async/pages */
#define MLX_MAX_EVENT_NOTIFIERS 4
#define MLX_MAX_EQ_PAGES        32

class MlxPCIDriver;

/*
 * Event callback (replaces atomic_notifier_call_chain)
 */
class MlxEventNotifier {
public:
    virtual void handleEvent(uint32_t type, void *eqe) = 0;
    virtual OSObject *notifierObject() = 0;
};

/*
 * EQE (Event Queue Element) 32 bytes — See device.h:769 struct mlx5_eqe
 */
struct MlxEqe {
    uint8_t  rsvd0;
    uint8_t  type;            /* event type */
    uint8_t  rsvd1;
    uint8_t  sub_type;
    uint32_t rsvd2[7];
    union {
        struct { uint32_t rsvd_c[6]; uint32_t cqn; } comp;  /* completion event */
        struct { uint32_t vector; uint32_t rsvd_cmd[6]; } cmd;
        struct {
            uint16_t ec_function;
            uint16_t function_id;
            uint32_t num_pages;
            uint32_t rsvd_page[5];
        } page_request;
    } data;
    uint16_t rsvd3;
    uint8_t  signature;
    uint8_t  owner;           /* ★ bit0: ownership bit */
} __attribute__((packed));

/*
 * EQ entry (one EQ ring buffer)
 */
struct MlxEqEntry {
    void        *ringBuf;       /* EQE ring buffer */
    uint64_t     ringDMA;
    uint32_t     logSize;       /* log2 of the depth */
    uint32_t     consIndex;     /* consumer index */
    uint32_t     eqNumber;
    bool         valid;
    uint32_t     doorbellOffset; /* UAR + MLX_EQ_DOORBELL */
    uint32_t     irqVector;
    IOBufferMemoryDescriptor *fDesc;  /* ring buffer memory descriptor */
    IODMACommand *fDmaMap;      /* retained IOMMU mapping */
    uint64_t     pageDMA[MLX_MAX_EQ_PAGES];
    uint32_t     numPages;
    uint64_t     eventMask[4];  /* event mask (256-bit) */
};

static_assert(sizeof(MlxEqe) == 64, "mlx5 EQE must be 64 bytes");

/*
 * Event queue management class
 */
class MlxEQ : public OSObject {
    OSDeclareDefaultStructors(MlxEQ)

public:
    bool init(MlxPCIDriver *owner, uint32_t numCompVectors);

    /* Create async EQs (cmd/async/pages) + completion EQs */
    kern_return_t createAsyncEqs();
    kern_return_t createCompEqs();

    /* Register MSI-X interrupts (IOInterruptEventSource) */
    kern_return_t setupInterrupts();
    void disableInterrupts();
    bool shutdown();
    void markHardwareStopped();
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* Subscribe to events (register for a specific type) */
    void registerNotifier(uint32_t eventType, MlxEventNotifier *n);
    void unregisterNotifier(uint32_t eventType, MlxEventNotifier *n);
    void synchronizeCallbacks();

    /* Interrupt handler entry points */
    static void asyncIntrHandler(OSObject *owner,
                                 IOInterruptEventSource *sender, int count);
    static void compIntrHandler(OSObject *owner,
                                IOInterruptEventSource *sender, int count);

    /* Completion EQ number for CQ binding */
    uint32_t getCompEqNumber(uint32_t vector) const
    {
        return (vector < fNumCompEqs) ? fCompEqs[vector].eqNumber : 0;
    }
    uint32_t getNumCompEqs() const { return fNumCompEqs; }

private:
    /* Create a single EQ (See eq.c:272 create_map_eq) */
    kern_return_t createEq(MlxEqEntry *eq, uint32_t vecidx,
                           const uint64_t mask[4]);

    /* Destroy an EQ */
    kern_return_t destroyEq(uint32_t eqNumber);

    /* Allocate the EQ ring buffer (paged, each slot owner set to 1) */
    kern_return_t allocEqBuf(MlxEqEntry *eq);

    void handleAsyncEqe(uint32_t eqIdx);
    void handleCompEqe(uint32_t eqIdx);

    /* Update CI + re-arm (See eq_update_ci) */
    void updateCi(MlxEqEntry *eq, bool arm);

    /* Check the ownership bit (See lib/eq.h:61) */
    bool isNewEqe(const MlxEqEntry *eq, const MlxEqe *eqe) const;

    MlxPCIDriver   *fOwner;
    MlxEqEntry     *fAsyncEqs;     /* MLX_NUM_ASYNC_EQS entries */
    uint32_t        fNumAsyncEqs;
    MlxEqEntry     *fCompEqs;      /* N completion EQs */
    uint32_t        fNumCompEqs;
    MlxEventNotifier *fNotifiers[MLX_EVENT_TYPE_MAX][MLX_MAX_EVENT_NOTIFIERS];
    uint32_t        fNotifierCounts[MLX_EVENT_TYPE_MAX];
    IOLock         *fNotifierLock;
    uint32_t        fActiveCallbacks;
    bool            fShuttingDown;
    IOWorkLoop     *fWorkLoop;
    IOInterruptEventSource *fAsyncIS;
    IOInterruptEventSource **fCompIS;
    bool             fInterruptsSetup;
};

#endif /* MLX_EQ_HPP */
