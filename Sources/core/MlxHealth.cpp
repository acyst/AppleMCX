/*
 * MlxHealth.cpp — Firmware health polling implementation (generic Mellanox family)
 *
 * Ported from: mlx5_core/health.c
 * Principle: firmware periodically increments iseg->health_counter;
 * the driver polls to check whether the counter changes, and declares the
 * firmware hung if it does not.
 */
#include "MlxHealth.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxKernelCompat.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxHealth, OSObject)

bool MlxHealth::init(MlxPCIDriver *owner)
{
    if (!super::init())
        return false;

    fOwner = owner;
    fState = kMlxHealthOk;
    fLastCounter = 0;
    fStarted = false;
    fTimer = NULL;
    fWorkLoop = NULL;
    fMissCount = 0;
    fSyndrome = 0;
    fExtendedSyndrome = 0;
    return true;
}

MlxHealthState MlxHealth::check()
{
    /* Read iseg->health_counter
     * See health.c: mlx5_health_check to check counter progress */
    uint32_t counter = mlxMMIORead32BE(
        fOwner->getBar0(), offsetof(struct MlxInitSeg, health_counter));

    if (fStarted && counter == fLastCounter) {
        if (++fMissCount >= 3) {
            fState = kMlxHealthWatchdog;
            IOLog("MlxHealth: firmware health counter stalled (counter=%u synd=%u ext=%u)\n",
                  counter, fSyndrome, fExtendedSyndrome);
            fOwner->enterDmaQuarantine(fSyndrome);
        }
    } else {
        fMissCount = 0;
        fState = kMlxHealthOk;
    }
    volatile uint8_t *health = reinterpret_cast<volatile uint8_t *>(
        static_cast<uintptr_t>(fOwner->getBar0()->getVirtualAddress()) +
        offsetof(struct MlxInitSeg, health));
    fSyndrome = health[offsetof(MlxInitSeg::MlxHealthBuffer, synd)];
    uint16_t ext = *reinterpret_cast<volatile uint16_t *>(
        health + offsetof(MlxInitSeg::MlxHealthBuffer, ext_synd));
    fExtendedSyndrome = OSSwapBigToHostInt16(ext);
    uint8_t severity = health[offsetof(MlxInitSeg::MlxHealthBuffer,
                                       rfr_severity)];
    if (fSyndrome && (severity & 0x80)) {
        fState = kMlxHealthWatchdog;
        fOwner->enterDmaQuarantine(fSyndrome);
    }
    fLastCounter = counter;
    return fState;
}

bool MlxHealth::start(uint64_t intervalUs)
{
    fIntervalUs = intervalUs;
    if (!fWorkLoop)
        fWorkLoop = IOWorkLoop::workLoop();
    if (fWorkLoop && !fTimer) {
        fTimer = IOTimerEventSource::timerEventSource(this,
            &MlxHealth::pollTimer);
        if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
            if (fTimer) fTimer->release();
            fTimer = NULL;
            return false;
        }
    }
    if (!fTimer)
        return false;
    fStarted = true;
    fLastCounter = mlxMMIORead32BE(
        fOwner->getBar0(), offsetof(struct MlxInitSeg, health_counter));
    fMissCount = 0;
    fTimer->setTimeoutUS(static_cast<UInt32>(intervalUs));
    IOLog("MlxHealth: health polling started (interval=%llu us)\n", intervalUs);
    return true;
}

void MlxHealth::stop()
{
    fStarted = false;
    if (fTimer)
        fTimer->cancelTimeout();
}

void MlxHealth::free()
{
    stop();
    if (fTimer) {
        if (fWorkLoop)
            fWorkLoop->removeEventSource(fTimer);
        fTimer->release();
        fTimer = NULL;
    }
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
    super::free();
}

void MlxHealth::pollTimer(OSObject *target, IOTimerEventSource *sender)
{
    MlxHealth *self = OSDynamicCast(MlxHealth, target);
    if (!self || !sender || !self->fStarted)
        return;
    self->check();
    if (self->fStarted)
        sender->setTimeoutUS(static_cast<UInt32>(self->fIntervalUs));
}
