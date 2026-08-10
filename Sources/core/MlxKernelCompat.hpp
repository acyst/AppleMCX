/*
 * MlxKernelCompat.hpp — Public macOS KEXT API compatibility helpers
 *
 * The public KEXT SDK does not expose Linux-style IORead32/IOWrite32 or
 * OSMemoryBarrier. Keep MMIO access and ordering in one place so the driver
 * does not depend on private IOKit APIs.
 */
#ifndef MLX_KERNEL_COMPAT_HPP
#define MLX_KERNEL_COMPAT_HPP

#include <stdint.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSByteOrder.h>
#include <IOKit/IOMemoryDescriptor.h>

/* Order regular memory accesses before/after DMA ownership transitions. */
static inline void
mlxMemoryBarrier()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* mlx5 init-segment registers use big-endian 32-bit MMIO accesses. */
static inline uint32_t
mlxMMIORead32BE(IOMemoryMap *map, uintptr_t offset)
{
    volatile uint32_t *reg = reinterpret_cast<volatile uint32_t *>(
        reinterpret_cast<uintptr_t>(map->getVirtualAddress()) + offset);
    uint32_t value = *reg;
    OSSynchronizeIO();
    return OSSwapBigToHostInt32(value);
}

static inline void
mlxMMIOWrite32BE(IOMemoryMap *map, uintptr_t offset, uint32_t value)
{
    volatile uint32_t *reg = reinterpret_cast<volatile uint32_t *>(
        reinterpret_cast<uintptr_t>(map->getVirtualAddress()) + offset);
    *reg = OSSwapHostToBigInt32(value);
    OSSynchronizeIO();
}

#endif /* MLX_KERNEL_COMPAT_HPP */
