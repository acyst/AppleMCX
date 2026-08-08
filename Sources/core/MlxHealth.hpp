/*
 * MlxHealth.hpp — Firmware health polling (generic Mellanox family)
 *
 * Ported from: mlx5_core/health.c + health_buffer (device.h:544)
 * Firmware periodically updates health_counter; the driver polls to detect a hang.
 */
#ifndef MLX_HEALTH_HPP
#define MLX_HEALTH_HPP

#include <libkern/OSTypes.h>
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
    void start(uint64_t intervalUs);
    void stop();

    /* Single health check (See health.c mlx5_health_check) */
    MlxHealthState check();

    /* Whether healthy */
    bool isHealthy() { return fState == kMlxHealthOk; }

private:
    /* Polling timer callback */
    static void pollTimer(OSObject *target, void *refCon);

    MlxPCIDriver *fOwner;
    uint64_t      fIntervalUs;
    MlxHealthState fState;
    uint32_t      fLastCounter;
    bool          fStarted;
    void         *fTimer;       /* timer handle (timer or thread on macOS) */
};

#endif /* MLX_HEALTH_HPP */
