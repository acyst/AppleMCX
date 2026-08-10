/*
 * MlxEQ.hpp — Event queues (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/net/ethernet/mellanox/mlx5/core/eq.c
 * macOS differences: use IOInterruptEventSource + IOWorkLoop instead of Linux tasklet/notifier
 */
#ifndef MLX_EQ_HPP
#define MLX_EQ_HPP

#include <libkern/OSTypes.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "MlxRegs.hpp"

#define MLX_EVENT_TYPE_MAX      64
#define MLX_NUM_ASYNC_EQS       3       /* cmd/async/pages */
#define MLX_NUM_SPARE_EQE       128     /* number of spare EQEs (See eq.h:35) */

class MlxPCIDriver;

/*
 * Event callback (replaces atomic_notifier_call_chain)
 */
class MlxEventNotifier {
public:
    virtual void handleEvent(uint32_t type, void *eqe) = 0;
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
        struct { uint32_t rsvd_cmd[6]; uint32_t vector; } cmd; /* command completion bitmap */
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
    uint32_t     doorbellOffset; /* UAR + MLX_EQ_DOORBELL */
    uint32_t     irqVector;
    IOBufferMemoryDescriptor *fDesc;  /* ring buffer memory descriptor */
    uint32_t     eventMask[4];  /* event mask (128-bit) */
};

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

    /* Subscribe to events (register for a specific type) */
    void registerNotifier(uint32_t eventType, MlxEventNotifier *n);
    void unregisterNotifier(uint32_t eventType, MlxEventNotifier *n);

    /* Interrupt handler entry points */
    static void asyncIntrHandler(OSObject *target, void *refCon,
                                 IOService *nub, int source);
    static void compIntrHandler(OSObject *target, void *refCon,
                                IOService *nub, int source);

    /* Completion EQ number for CQ binding */
    uint32_t getCompEqNumber(uint32_t vector) const
    {
        return (vector < fNumCompEqs) ? fCompEqs[vector].eqNumber : 0;
    }

private:
    /* Create a single EQ (See eq.c:272 create_map_eq) */
    kern_return_t createEq(MlxEqEntry *eq, uint32_t vecidx,
                           const uint32_t mask[4]);

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
    OSArray        *fNotifiers[MLX_EVENT_TYPE_MAX];
    IOLock         *fNotifierLock;
    IOWorkLoop     *fWorkLoop;
    IOInterruptEventSource *fAsyncIS;
    IOInterruptEventSource **fCompIS;
};

#endif /* MLX_EQ_HPP */
