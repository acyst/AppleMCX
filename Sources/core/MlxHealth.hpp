/*
 * MlxHealth.hpp — Firmware health polling (generic Mellanox family)
 *
 * Ported from: mlx5_core/health.c + health_buffer (device.h:544)
 * Firmware periodically updates health_counter; the driver polls to detect a hang.
 */
#ifndef MLX_HEALTH_HPP
#define MLX_HEALTH_HPP

#include <libkern/OSTypes.h>
#include <libkern/c++/OSObject.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "MlxRegs.hpp"

class MlxPCIDriver;

/*
 * Health state
 */
enum MlxHealthState {
    kMlxHealthOk = 0,
    kMlxHealthWatchdog = 1,    /* firmware hung */
    kMlxHealthRecovered = 2,
};

/*
 * Health monitoring class
 */
class MlxHealth : public OSObject {
    OSDeclareDefaultStructors(MlxHealth)

public:
    bool init(MlxPCIDriver *owner);

    /* Start polling (interval in microseconds) */
    bool start(uint64_t intervalUs);
    void stop();

    /* Single health check (See health.c mlx5_health_check) */
    MlxHealthState check();
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* Whether healthy */
    bool isHealthy() { return fState == kMlxHealthOk; }

private:
    /* Polling timer callback */
    static void pollTimer(OSObject *target, IOTimerEventSource *sender);

    MlxPCIDriver *fOwner;
    uint64_t      fIntervalUs;
    MlxHealthState fState;
    uint32_t      fLastCounter;
    uint32_t      fMissCount;
    uint8_t       fSyndrome;
    uint16_t      fExtendedSyndrome;
    bool          fStarted;
    IOWorkLoop    *fWorkLoop;
    IOTimerEventSource *fTimer;
};

#endif /* MLX_HEALTH_HPP */
