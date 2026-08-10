/*
 * MlxHealth.cpp — Firmware health polling implementation (generic Mellanox family)
 *
 * Ported from: mlx5_core/health.c
 * Principle: firmware periodically increments iseg->health.hw_health_counter;
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
    return true;
}

MlxHealthState MlxHealth::check()
{
    /* Read iseg->health.hw_health_counter
     * See health.c: mlx5_health_check to check counter progress */
    uint32_t counter = mlxMMIORead32BE(
        fOwner->getBar0(), offsetof(struct MlxInitSeg, health) +
        offsetof(MlxInitSeg::MlxHealthBuffer, hw_health_counter));

    if (fStarted && counter == fLastCounter) {
        /* Counter unchanged → firmware may be hung (MVP: record only, full recovery P5) */
        fState = kMlxHealthWatchdog;
        IOLog("MlxHealth: firmware health counter not updated (counter=%u), possible hang\n",
              counter);
    } else {
        fState = kMlxHealthOk;
    }
    fLastCounter = counter;
    return fState;
}

void MlxHealth::start(uint64_t intervalUs)
{
    fIntervalUs = intervalUs;
    fStarted = true;
    fLastCounter = mlxMMIORead32BE(
        fOwner->getBar0(), offsetof(struct MlxInitSeg, health) +
        offsetof(MlxInitSeg::MlxHealthBuffer, hw_health_counter));
    IOLog("MlxHealth: health polling started (interval=%llu us)\n", intervalUs);
    /* MVP: polling uses a timer; full implementation in late phase P1 */
}

void MlxHealth::stop()
{
    fStarted = false;
}

void MlxHealth::pollTimer(OSObject *target, void *refCon)
{
    MlxHealth *self = (MlxHealth *)refCon;
    self->check();
}
