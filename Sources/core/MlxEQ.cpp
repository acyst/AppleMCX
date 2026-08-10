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

/* Initialize the owner bit of each EQE in the ring buffer */
static void
init_eq_buf(MlxEqEntry *eq)
{
    /* See eq.c:251 init_eq_buf: set the owner bit to 1 for each slot */
    uint8_t *buf = (uint8_t *)eq->ringBuf;
    uint32_t size = 1u << eq->logSize;
    for (uint32_t i = 0; i < size; i++) {
        MlxEqe *eqe = (MlxEqe *)(buf + (i * 32));
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

    for (int i = 0; i < MLX_EVENT_TYPE_MAX; i++)
        fNotifiers[i] = OSArray::withCapacity(4);

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        return false;

    return true;
}

kern_return_t MlxEQ::allocEqBuf(MlxEqEntry *eq)
{
    /* Allocate the EQ ring buffer (32B EQE * depth + spares) */
    uint32_t sizeBytes = (1u << eq->logSize) + MLX_NUM_SPARE_EQE;
    sizeBytes *= 32;

    eq->fDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, sizeBytes, 0xFFFFFFF000ULL);
    if (!eq->fDesc)
        return kIOReturnNoMemory;
    if (eq->fDesc->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return kIOReturnNoMemory;

    eq->ringBuf = eq->fDesc->getBytesNoCopy();
    eq->ringDMA = eq->fDesc->getPhysicalSegment(0, 0);
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

    /* Build the CREATE_EQ command:
     * create_eq_in = 64B header + eqc(192B) + event_bitmask(128B) + pas[] */
    uint8_t in[4096] = {};
    uint32_t eqcOffset = 0x40;                    /* 64-byte command header */
    uint32_t maskOffset = eqcOffset + 192;        /* eqc is 192 bytes */
    uint32_t pasOffset = maskOffset + 128;        /* event_bitmask is 128 bytes */

    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_EQ);

    /* eqc (See mlx5_ifc.h:4258 mlx5_ifc_eqc_bits, bitfield layout):
     *   st[0x10](4bit) → bit 0x10 = byte 0x2 bit0, 4 bits → low nibble of byte 0x2
     *   log_eq_size[0x63](5bit) → bit 0x63 = byte 0xC bit3, 5 bits → byte 0xC bits[7:3]
     *   uar_page[0x68](24bit) → bit 0x68 = byte 0xD bit0, 24 bits → bytes 0xD/0xE/0xF
     *   intr[0xB4](12bit) → bit 0xB4 = byte 0x16 bit4, 12 bits → byte 0x16 bits[7:4] + byte 0x17 */
    uint8_t *eqc = in + eqcOffset;
    /* st = 0x4 (EQ), 4 bits starting at bit 0x10 */
    eqc[0x02] = (uint8_t)((eqc[0x02] & 0xF0) | 0x4);
    /* log_eq_size: 5 bits starting at bit 0x63 → byte 0xC bits[7:3] */
    eqc[0x0C] = (uint8_t)((eq->logSize << 3) & 0xF8);
    /* uar_page: 24 bits starting at bit 0x68 → bytes 0xD/0xE/0xF */
    uint32_t uarPage = fOwner->getUAR() ? fOwner->getUAR()->getBootUarIndex() : 0;
    eqc[0x0D] = (uint8_t)((uarPage >> 16) & 0xFF);
    eqc[0x0E] = (uint8_t)((uarPage >> 8) & 0xFF);
    eqc[0x0F] = (uint8_t)(uarPage & 0xFF);
    /* intr: 12 bits starting at bit 0xB4 → byte 0x16 bits[7:4] + byte 0x17 */
    eqc[0x16] = (uint8_t)((vecidx << 4) & 0xF0);
    eqc[0x17] = (uint8_t)((vecidx >> 4) & 0xFF);
    /* log_page_size: 5 bits starting at bit 0xD8 → byte 0x1B bits[7:3] (PAGE_SHIFT=12 → 0) */
    eqc[0x1B] = (uint8_t)((0 << 3) & 0xF8);

    /* event_bitmask (128-bit) */
    for (int i = 0; i < 4; i++)
        OSWriteBigInt32(in, maskOffset + (i * 4), mask[i]);

    /* PAS (physical address list, 8 bytes each) */
    uint32_t numPages = 1;   /* MVP: single-page buffer */
    for (uint32_t i = 0; i < numPages; i++)
        OSWriteBigInt64(in, pasOffset + (i * 8), eq->ringDMA);

    uint32_t inSize = pasOffset + (numPages * 8);
    uint8_t out[64] = {};

    MlxCmdInOut cmd = { in, inSize, out, sizeof(out), MLX_CMD_OP_CREATE_EQ };
    kr = fOwner->getCmd()->exec(&cmd, 5000);
    if (kr != kIOReturnSuccess) {
        eq->fDesc->complete();
        eq->fDesc->release();
        eq->fDesc = NULL;
        return kr;
    }

    /* Read back eq_number (create_eq_out, offset 0x40+24) */
    eq->eqNumber = OSReadBigInt32(out, 0x40 + 24) & 0xFF;
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
    OSWriteBigInt32(in, 4, eqNumber);
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
    if (MLX_EVENT_TYPE_CMD >= 32)
        cmdMask[1] = 1u << (MLX_EVENT_TYPE_CMD - 32);
    cmdMask[0] |= 1u << (MLX_EVENT_TYPE_PAGE_REQUEST & 31);
    if (MLX_EVENT_TYPE_PAGE_REQUEST >= 32)
        cmdMask[1] |= 1u << (MLX_EVENT_TYPE_PAGE_REQUEST - 32);

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
        kern_return_t kr = createEq(&fCompEqs[i], i, compMask);
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
    if (fNotifiers[eventType] && !fNotifiers[eventType]->containsObject(n))
        fNotifiers[eventType]->setObject(n);
    IOLockUnlock(fNotifierLock);
}

void MlxEQ::unregisterNotifier(uint32_t eventType, MlxEventNotifier *n)
{
    if (eventType >= MLX_EVENT_TYPE_MAX)
        return;
    IOLockLock(fNotifierLock);
    if (fNotifiers[eventType])
        fNotifiers[eventType]->removeObject(n);
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
    /* MVP: doorbell accessed via UAR, implemented after ioremap in phase P1 */
    (void)ci;
    (void)arm;
}

void MlxEQ::handleAsyncEqe(uint32_t eqIdx)
{
    MlxEqEntry *eq = &fAsyncEqs[eqIdx];
    uint8_t *buf = (uint8_t *)eq->ringBuf;
    uint32_t size = 1u << eq->logSize;

    for (uint32_t i = 0; i < size; i++) {
        MlxEqe *eqe = (MlxEqe *)(buf + ((eq->consIndex & (size - 1)) * 32));
        if (!isNewEqe(eq, eqe))
            break;
        mlxMemoryBarrier();
        /* Dispatch to the notifier list (replaces atomic_notifier_call_chain) */
        uint32_t type = eqe->type;
        IOLockLock(fNotifierLock);
        OSArray *list = (type < MLX_EVENT_TYPE_MAX) ? fNotifiers[type] : NULL;
        if (list) {
            for (uint32_t n = 0; n < list->getCount(); n++) {
                MlxEventNotifier *nb = (MlxEventNotifier *)list->getObject(n);
                if (nb)
                    nb->handleEvent(type, eqe);
            }
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
        MlxEqe *eqe = (MlxEqe *)(buf + ((eq->consIndex & (size - 1)) * 32));
        if (!isNewEqe(eq, eqe))
            break;
        mlxMemoryBarrier();
        /* CQ completion event (See mlx5_eq_comp_int, eq.c:106)
         * eqe->data.comp.cqn → CQ completion callback
         * MVP: dispatch to the COMPLETION notifier */
        IOLockLock(fNotifierLock);
        OSArray *list = fNotifiers[MLX_EVENT_TYPE_COMPLETION];
        if (list) {
            for (uint32_t n = 0; n < list->getCount(); n++) {
                MlxEventNotifier *nb = (MlxEventNotifier *)list->getObject(n);
                if (nb)
                    nb->handleEvent(MLX_EVENT_TYPE_COMPLETION, eqe);
            }
        }
        IOLockUnlock(fNotifierLock);
        eq->consIndex++;
    }
    updateCi(eq, true);
}

void MlxEQ::asyncIntrHandler(OSObject *target, void *refCon,
                             IOService *nub, int source)
{
    MlxEQ *self = (MlxEQ *)refCon;
    self->handleAsyncEqe((uint32_t)source);
}

void MlxEQ::compIntrHandler(OSObject *target, void *refCon,
                            IOService *nub, int source)
{
    MlxEQ *self = (MlxEQ *)refCon;
    self->handleCompEqe((uint32_t)source);
}

kern_return_t MlxEQ::setupInterrupts()
{
    /* See mlx5_eq_enable (eq.c:372) + IRQ registration
     * async EQs share interrupt vector 0, each completion EQ gets its own vector */
    if (!fWorkLoop)
        return kIOReturnNoResources;

    /* Async interrupt source */
    fAsyncIS = IOInterruptEventSource::interruptEventSource(
        this,
        OSMemberFunctionCast(IOInterruptEventAction, this,
                             &MlxEQ::asyncIntrHandler),
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
            this,
            OSMemberFunctionCast(IOInterruptEventAction, this,
                                 &MlxEQ::compIntrHandler),
            fOwner->getPCI(), i + 1);   /* vectors 1..N */
        if (fCompIS[i]) {
            if (fWorkLoop->addEventSource(fCompIS[i]) != kIOReturnSuccess)
                return kIOReturnError;
            fCompIS[i]->enable();
        }
    }
    return kIOReturnSuccess;
}
