/*
 * MlxEQ.cpp — Event queue implementation (generic Mellanox family)
 *
 * Ported from: mlx5_core/eq.c
 * Key points:
 *   - CREATE_EQ command (eq.c:272 create_map_eq): eqc + event_bitmask + PAS
 *   - EQE is 32 bytes, owner bit in bit0 of the last byte (device.h:769)
 *   - CI update doorbell (lib/eq.h:68)
 * macOS differences: IOInterruptEventSource + IOWorkLoop instead of Linux tasklet/notifier
 */
#include "MlxEQ.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"

#include <string.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSByteOrder.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxEQ, OSObject)

static void
free_eq_entry(MlxEqEntry *eq)
{
    if (!eq)
        return;
    mlxUnmapDMA(eq->fDmaMap);
    eq->fDmaMap = NULL;
    if (eq->fDesc) {
        eq->fDesc->release();
        eq->fDesc = NULL;
    }
    eq->ringBuf = NULL;
}

bool MlxEQ::shutdown()
{
    bool allDestroyed = true;
    if (fAsyncIS) {
        fAsyncIS->disable();
        if (fWorkLoop) fWorkLoop->removeEventSource(fAsyncIS);
        fAsyncIS->release();
        fAsyncIS = NULL;
    }
    if (fCompIS) {
        for (uint32_t i = 0; i < fNumCompEqs; i++) {
            if (!fCompIS[i])
                continue;
            fCompIS[i]->disable();
            if (fWorkLoop) fWorkLoop->removeEventSource(fCompIS[i]);
            fCompIS[i]->release();
            fCompIS[i] = NULL;
        }
    }

    if (fOwner && fOwner->getCmd() && fOwner->getCmd()->isUp()) {
        for (uint32_t i = 0; i < fNumCompEqs; i++) {
            if (fCompEqs && fCompEqs[i].valid) {
                if (destroyEq(fCompEqs[i].eqNumber) == kIOReturnSuccess)
                    fCompEqs[i].valid = false;
                else
                    allDestroyed = false;
            }
        }
        for (uint32_t i = 0; i < fNumAsyncEqs; i++) {
            if (fAsyncEqs && fAsyncEqs[i].valid) {
                if (destroyEq(fAsyncEqs[i].eqNumber) == kIOReturnSuccess)
                    fAsyncEqs[i].valid = false;
                else
                    allDestroyed = false;
            }
        }
    } else {
        for (uint32_t i = 0; i < fNumCompEqs; i++)
            allDestroyed &= !fCompEqs || !fCompEqs[i].valid;
        for (uint32_t i = 0; i < fNumAsyncEqs; i++)
            allDestroyed &= !fAsyncEqs || !fAsyncEqs[i].valid;
    }
    return allDestroyed;
}

void MlxEQ::markHardwareStopped()
{
    for (uint32_t i = 0; i < fNumCompEqs; i++) {
        if (fCompEqs)
            fCompEqs[i].valid = false;
    }
    for (uint32_t i = 0; i < fNumAsyncEqs; i++) {
        if (fAsyncEqs)
            fAsyncEqs[i].valid = false;
    }
}

void MlxEQ::free()
{
    shutdown();
    if (fCompEqs) {
        for (uint32_t i = 0; i < fNumCompEqs; i++) {
            if (!fCompEqs[i].valid)
                free_eq_entry(&fCompEqs[i]);
        }
        IOFree(fCompEqs, sizeof(MlxEqEntry) * fNumCompEqs);
        fCompEqs = NULL;
    }
    if (fAsyncEqs) {
        for (uint32_t i = 0; i < fNumAsyncEqs; i++) {
            if (!fAsyncEqs[i].valid)
                free_eq_entry(&fAsyncEqs[i]);
        }
        IOFree(fAsyncEqs, sizeof(MlxEqEntry) * fNumAsyncEqs);
        fAsyncEqs = NULL;
    }
    if (fCompIS) {
        IOFree(fCompIS, sizeof(IOInterruptEventSource *) * fNumCompEqs);
        fCompIS = NULL;
    }
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
    if (fNotifierLock) {
        IOLockFree(fNotifierLock);
        fNotifierLock = NULL;
    }
    super::free();
}

/* Initialize the owner bit of each EQE in the ring buffer */
static void
init_eq_buf(MlxEqEntry *eq)
{
    /* See eq.c:251 init_eq_buf: set the owner bit to 1 for each slot */
    uint8_t *buf = (uint8_t *)eq->ringBuf;
    uint32_t size = 1u << eq->logSize;
    for (uint32_t i = 0; i < size; i++) {
        MlxEqe *eqe = (MlxEqe *)(buf + (i * sizeof(MlxEqe)));
        eqe->owner = 1;
    }
    mlxMemoryBarrier();
}

bool MlxEQ::init(MlxPCIDriver *owner, uint32_t numCompVectors)
{
    if (!super::init())
        return false;

    fOwner = owner;
    fNumAsyncEqs = MLX_NUM_ASYNC_EQS;    /* cmd/async/pages */
    fNumCompEqs = numCompVectors;
    fAsyncEqs = NULL;
    fCompEqs = NULL;
    fAsyncIS = NULL;
    fCompIS = NULL;

    fAsyncEqs = (MlxEqEntry *)IOMallocZero(sizeof(MlxEqEntry) * fNumAsyncEqs);
    fCompEqs = (MlxEqEntry *)IOMallocZero(sizeof(MlxEqEntry) * fNumCompEqs);
    fNotifierLock = IOLockAlloc();
    if (!fAsyncEqs || !fCompEqs || !fNotifierLock)
        return false;

    memset(fNotifiers, 0, sizeof(fNotifiers));
    memset(fNotifierCounts, 0, sizeof(fNotifierCounts));

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        return false;

    return true;
}

kern_return_t MlxEQ::allocEqBuf(MlxEqEntry *eq)
{
    /* The PAS geometry must describe exactly the depth reported in EQC. */
    uint32_t sizeBytes = (1u << eq->logSize) * sizeof(MlxEqe);
    sizeBytes = (sizeBytes + 4095) & ~4095u;

    eq->fDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, sizeBytes, 0xFFFFFFF000ULL);
    if (!eq->fDesc)
        return kIOReturnNoMemory;
    eq->ringBuf = eq->fDesc->getBytesNoCopy();
    eq->fDmaMap = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 4096, IODMACommand::kMapped,
        sizeBytes, 4096);
    if (!eq->fDmaMap ||
        eq->fDmaMap->setMemoryDescriptor(eq->fDesc) != kIOReturnSuccess) {
        if (eq->fDmaMap) eq->fDmaMap->release();
        eq->fDmaMap = NULL;
        eq->fDesc->release();
        eq->fDesc = NULL;
        return kIOReturnNoMemory;
    }

    UInt64 offset = 0;
    eq->numPages = 0;
    while (offset < sizeBytes && eq->numPages < MLX_MAX_EQ_PAGES) {
        IODMACommand::Segment64 segments[8];
        UInt32 count = 8;
        IOReturn kr = eq->fDmaMap->gen64IOVMSegments(&offset, segments, &count);
        if (kr != kIOReturnSuccess || count == 0)
            break;
        for (UInt32 i = 0; i < count && eq->numPages < MLX_MAX_EQ_PAGES; i++) {
            if ((segments[i].fIOVMAddr & 0xFFF) || segments[i].fLength > 4096)
                break;
            eq->pageDMA[eq->numPages++] = segments[i].fIOVMAddr;
        }
    }
    if (offset != sizeBytes || eq->numPages == 0) {
        mlxUnmapDMA(eq->fDmaMap);
        eq->fDmaMap = NULL;
        eq->fDesc->release();
        eq->fDesc = NULL;
        return kIOReturnNoSpace;
    }
    eq->ringDMA = eq->pageDMA[0];
    memset(eq->ringBuf, 0, sizeBytes);
    init_eq_buf(eq);
    return kIOReturnSuccess;
}

kern_return_t MlxEQ::createEq(MlxEqEntry *eq, uint32_t vecidx,
                              const uint32_t mask[4])
{
    /* See eq.c:272 create_map_eq */
    kern_return_t kr = allocEqBuf(eq);
    if (kr != kIOReturnSuccess)
        return kr;

    /* CREATE_EQ IFC: EQC@0x80 bits, event mask@0x2c0, PAS@0x880. */
    uint8_t in[4096] = {};
    uint32_t eqcOffset = 0x80 / 8;
    uint32_t maskOffset = 0x2c0 / 8;
    uint32_t pasOffset = 0x880 / 8;

    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_EQ);

    /* eqc (See mlx5_ifc.h mlx5_ifc_eqc_bits):
     *   log_eq_size[0x63](5bit) → bit 0x63 = byte 0xC bit3
     *   uar_page[0x68](24bit) → bit 0x68 = byte 0xD bit0, 24 bits → bytes 0xD/0xE/0xF
     *   intr[0xB4](12bit) → bit 0xB4 = byte 0x16 bit4, 12 bits → byte 0x16 bits[7:4] + byte 0x17 */
    uint8_t *eqc = in + eqcOffset;
    mlxSetBits(eqc, 0x63, 5, eq->logSize);
    uint32_t uarPage = fOwner->getUAR() ? fOwner->getUAR()->getBootUarIndex() : 0;
    mlxSetBits(eqc, 0x68, 24, uarPage);
    mlxSetBits(eqc, 0xb4, 12, vecidx);
    mlxSetBits(eqc, 0xc3, 5, 0); /* 4 KiB pages */

    /* event_bitmask (128-bit) */
    for (uint32_t i = 0; i < 4; i++)
        OSWriteBigInt32(in, maskOffset + (i * 4), mask[i]);

    /* PAS (physical address list, 8 bytes each) */
    uint32_t numPages = eq->numPages;
    for (uint32_t i = 0; i < numPages; i++)
        OSWriteBigInt64(in, pasOffset + (i * 8), eq->pageDMA[i]);

    uint32_t inSize = pasOffset + (numPages * 8);
    uint8_t out[64] = {};

    MlxCmdInOut cmd = { in, inSize, out, sizeof(out), MLX_CMD_OP_CREATE_EQ };
    kr = fOwner->getCmd()->exec(&cmd, 5000);
    if (kr != kIOReturnSuccess) {
        eq->fDmaMap->clearMemoryDescriptor();
        eq->fDmaMap->release();
        eq->fDmaMap = NULL;
        eq->fDesc->release();
        eq->fDesc = NULL;
        return kr;
    }

    eq->eqNumber = static_cast<uint32_t>(mlxGetBits(out, 0x58, 8));
    eq->valid = true;
    eq->irqVector = vecidx;
    eq->doorbellOffset = MLX_EQ_DOORBELL;
    eq->consIndex = 0;

    IOLog("MlxEQ: EQ created (eqn=%u, log_size=%u, vec=%u)\n",
          eq->eqNumber, eq->logSize, vecidx);
    return kIOReturnSuccess;
}

kern_return_t MlxEQ::destroyEq(uint32_t eqNumber)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_EQ);
    mlxSetBits(in, 0x58, 8, eqNumber);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_DESTROY_EQ };
    return fOwner->getCmd()->exec(&cmd, 5000);
}

kern_return_t MlxEQ::createAsyncEqs()
{
    /* See create_async_eqs (eq.c:696)
     * [0] cmd_eq: CMD events only
     * [1] async_eq: generic async events
     * [2] pages_eq: PAGE_REQUEST */
    uint32_t cmdMask[4] = {};
    cmdMask[0] = 1u << (MLX_EVENT_TYPE_CMD & 31);
    cmdMask[0] |= 1u << (MLX_EVENT_TYPE_PAGE_REQUEST & 31);

    fAsyncEqs[0].logSize = 8;    /* 256 entries */
    kern_return_t kr = createEq(&fAsyncEqs[0], 0, cmdMask);
    if (kr != kIOReturnSuccess)
        return kr;

    /* async_eq: generic mask (MVP: accept most async events) */
    uint32_t asyncMask[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    fAsyncEqs[1].logSize = 8;
    kr = createEq(&fAsyncEqs[1], 0, asyncMask);
    if (kr != kIOReturnSuccess)
        return kr;

    /* pages_eq: PAGE_REQUEST only */
    uint32_t pagesMask[4] = {};
    pagesMask[0] = 1u << (MLX_EVENT_TYPE_PAGE_REQUEST & 31);
    fAsyncEqs[2].logSize = 0;    /* depth 1 */
    kr = createEq(&fAsyncEqs[2], 0, pagesMask);
    if (kr != kIOReturnSuccess)
        return kr;

    return kIOReturnSuccess;
}

kern_return_t MlxEQ::createCompEqs()
{
    /* See create_comp_eqs (eq.c:969) */
    uint32_t compMask[4] = {};
    compMask[0] = 1u << MLX_EVENT_TYPE_COMPLETION;

    for (uint32_t i = 0; i < fNumCompEqs; i++) {
        fCompEqs[i].logSize = 10;    /* 1024 entries */
        kern_return_t kr = createEq(&fCompEqs[i], i + 1, compMask);
        if (kr != kIOReturnSuccess)
            return kr;
    }
    return kIOReturnSuccess;
}

void MlxEQ::registerNotifier(uint32_t eventType, MlxEventNotifier *n)
{
    if (eventType >= MLX_EVENT_TYPE_MAX)
        return;
    IOLockLock(fNotifierLock);
    uint32_t count = fNotifierCounts[eventType];
    for (uint32_t i = 0; i < count; i++) {
        if (fNotifiers[eventType][i] == n) {
            IOLockUnlock(fNotifierLock);
            return;
        }
    }
    if (count < MLX_MAX_EVENT_NOTIFIERS) {
        fNotifiers[eventType][count] = n;
        fNotifierCounts[eventType] = count + 1;
    }
    IOLockUnlock(fNotifierLock);
}

void MlxEQ::unregisterNotifier(uint32_t eventType, MlxEventNotifier *n)
{
    if (eventType >= MLX_EVENT_TYPE_MAX)
        return;
    IOLockLock(fNotifierLock);
    uint32_t count = fNotifierCounts[eventType];
    for (uint32_t i = 0; i < count; i++) {
        if (fNotifiers[eventType][i] != n)
            continue;
        for (uint32_t j = i + 1; j < count; j++)
            fNotifiers[eventType][j - 1] = fNotifiers[eventType][j];
        fNotifiers[eventType][count - 1] = NULL;
        fNotifierCounts[eventType] = count - 1;
        break;
    }
    IOLockUnlock(fNotifierLock);
}

bool MlxEQ::isNewEqe(const MlxEqEntry *eq, const MlxEqe *eqe) const
{
    /* See lib/eq.h:61:
     *   (eqe->owner ^ (cons_index >> log_sz)) & 1 == 0 means a new event */
    uint32_t owner = eqe->owner & 1;
    uint32_t expected = (eq->consIndex >> eq->logSize) & 1;
    return (owner ^ expected) == 0;
}

void MlxEQ::updateCi(MlxEqEntry *eq, bool arm)
{
    /* See lib/eq.h:68-76:
     *   write cons_index (low 24 bits) + eqn<<24 to the doorbell
     *   write +0x40 (MLX_EQ_DOORBELL) when armed, otherwise +0x42 */
    uint32_t ci = (eq->consIndex & 0xFFFFFF) | (eq->eqNumber << 24);
    MlxUAR *uar = fOwner ? fOwner->getUAR() : NULL;
    IOMemoryMap *map = uar ? uar->getUarMap() : NULL;
    if (!map)
        return;
    uint32_t offset = eq->doorbellOffset + (arm ? 0 : 8);
    mlxMMIOWrite32BE(map, offset, ci);
}

void MlxEQ::handleAsyncEqe(uint32_t eqIdx)
{
    MlxEqEntry *eq = &fAsyncEqs[eqIdx];
    uint8_t *buf = (uint8_t *)eq->ringBuf;
    uint32_t size = 1u << eq->logSize;

    for (uint32_t i = 0; i < size; i++) {
        MlxEqe *eqe = (MlxEqe *)(buf +
            ((eq->consIndex & (size - 1)) * sizeof(MlxEqe)));
        if (!isNewEqe(eq, eqe))
            break;
        mlxMemoryBarrier();
        /* Dispatch to the notifier list (replaces atomic_notifier_call_chain) */
        uint32_t type = eqe->type;
        IOLockLock(fNotifierLock);
        uint32_t count = (type < MLX_EVENT_TYPE_MAX) ?
            fNotifierCounts[type] : 0;
        for (uint32_t n = 0; n < count; n++) {
            MlxEventNotifier *nb = fNotifiers[type][n];
            if (nb)
                nb->handleEvent(type, eqe);
        }
        IOLockUnlock(fNotifierLock);
        eq->consIndex++;
    }
    updateCi(eq, true);
}

void MlxEQ::handleCompEqe(uint32_t eqIdx)
{
    MlxEqEntry *eq = &fCompEqs[eqIdx];
    uint8_t *buf = (uint8_t *)eq->ringBuf;
    uint32_t size = 1u << eq->logSize;

    for (uint32_t i = 0; i < size; i++) {
        MlxEqe *eqe = (MlxEqe *)(buf +
            ((eq->consIndex & (size - 1)) * sizeof(MlxEqe)));
        if (!isNewEqe(eq, eqe))
            break;
        mlxMemoryBarrier();
        /* CQ completion event (See mlx5_eq_comp_int, eq.c:106)
         * eqe->data.comp.cqn → CQ completion callback
         * MVP: dispatch to the COMPLETION notifier */
        IOLockLock(fNotifierLock);
        uint32_t count = fNotifierCounts[MLX_EVENT_TYPE_COMPLETION];
        for (uint32_t n = 0; n < count; n++) {
            MlxEventNotifier *nb =
                fNotifiers[MLX_EVENT_TYPE_COMPLETION][n];
            if (nb)
                nb->handleEvent(MLX_EVENT_TYPE_COMPLETION, eqe);
        }
        IOLockUnlock(fNotifierLock);
        eq->consIndex++;
    }
    updateCi(eq, true);
}

void MlxEQ::asyncIntrHandler(OSObject *owner,
                             IOInterruptEventSource *sender, int count)
{
    (void)sender;
    (void)count;
    MlxEQ *self = OSDynamicCast(MlxEQ, owner);
    if (!self)
        return;
    /* Vector 0 is shared by the command, async, and page-request EQs. */
    for (uint32_t i = 0; i < self->fNumAsyncEqs; i++)
        self->handleAsyncEqe(i);
}

void MlxEQ::compIntrHandler(OSObject *owner,
                            IOInterruptEventSource *sender, int count)
{
    (void)count;
    MlxEQ *self = OSDynamicCast(MlxEQ, owner);
    if (!self || !sender)
        return;
    int vector = sender->getIntIndex();
    if (vector > 0 && static_cast<uint32_t>(vector) <= self->fNumCompEqs)
        self->handleCompEqe(static_cast<uint32_t>(vector - 1));
}

kern_return_t MlxEQ::setupInterrupts()
{
    /* See mlx5_eq_enable (eq.c:372) + IRQ registration
     * async EQs share interrupt vector 0, each completion EQ gets its own vector */
    if (!fWorkLoop)
        return kIOReturnNoResources;

    /* Async interrupt source */
    fAsyncIS = IOInterruptEventSource::interruptEventSource(
        this, &MlxEQ::asyncIntrHandler,
        fOwner->getPCI(), 0);
    if (fAsyncIS && fWorkLoop->addEventSource(fAsyncIS) == kIOReturnSuccess)
        fAsyncIS->enable();
    else
        return kIOReturnError;

    /* Completion interrupt sources (one per vector) */
    fCompIS = (IOInterruptEventSource **)
        IOMallocZero(sizeof(IOInterruptEventSource *) * fNumCompEqs);
    if (!fCompIS)
        return kIOReturnNoMemory;

    for (uint32_t i = 0; i < fNumCompEqs; i++) {
        fCompIS[i] = IOInterruptEventSource::interruptEventSource(
            this, &MlxEQ::compIntrHandler,
            fOwner->getPCI(), (int)(i + 1));   /* vectors 1..N */
        if (fCompIS[i]) {
            if (fWorkLoop->addEventSource(fCompIS[i]) != kIOReturnSuccess)
                return kIOReturnError;
            fCompIS[i]->enable();
        }
    }
    return kIOReturnSuccess;
}
