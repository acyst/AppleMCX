/*
 * perftest_lat.h — latency test common framework (macOS) — perftest equivalent
 *
 * Provides the common flow for latency tests: device/PD/MR/CQ/QP creation + single-shot latency measurement + statistics.
 */
#ifndef PERFTEST_LAT_H
#define PERFTEST_LAT_H

#include "perftest_common.h"

/* Latency statistics */
struct pt_lat_stats {
    double min_us;
    double max_us;
    double total_us;
    int    count;
};

static inline void
lat_init(struct pt_lat_stats *s)
{
    s->min_us = 1e9;
    s->max_us = 0;
    s->total_us = 0;
    s->count = 0;
}

static inline void
lat_add(struct pt_lat_stats *s, double us)
{
    if (us < s->min_us) s->min_us = us;
    if (us > s->max_us) s->max_us = us;
    s->total_us += us;
    s->count++;
}

static inline void
lat_print(const char *label, struct pt_lat_stats *s)
{
    printf("\n%s results:\n", label);
    printf("  Samples: %d\n", s->count);
    if (s->count == 0) {
        printf("  No valid samples\n");
        return;
    }
    printf("  Average latency: %.2f us\n", s->total_us / s->count);
    printf("  Minimum latency: %.2f us\n", s->min_us);
    printf("  Maximum latency: %.2f us\n", s->max_us);
}

#endif /* PERFTEST_LAT_H */
