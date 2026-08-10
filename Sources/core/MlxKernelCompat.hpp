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
#include <libkern/c++/OSData.h>
#include <IOKit/IOMemoryDescriptor.h>

/* Order regular memory accesses before/after DMA ownership transitions. */
static inline void
mlxMemoryBarrier()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* OSArray only stores OSObject instances; use OSData for plain records. */
template <typename T>
static inline T *
mlxRecordValue(OSObject *object)
{
    OSData *record = OSDynamicCast(OSData, object);
    if (!record || record->getLength() != sizeof(T))
        return NULL;
    return static_cast<T *>(const_cast<void *>(record->getBytesNoCopy()));
}

/* Convert an IOVirtualAddress to a typed kernel pointer (integer→pointer). */
static inline volatile uint32_t *
mlxVirtToReg(IOVirtualAddress vaddr, uintptr_t offset)
{
    uintptr_t addr = static_cast<uintptr_t>(vaddr) + offset;
    return reinterpret_cast<volatile uint32_t *>(addr);
}

/* mlx5 init-segment registers use big-endian 32-bit MMIO accesses. */
static inline uint32_t
mlxMMIORead32BE(IOMemoryMap *map, uintptr_t offset)
{
    volatile uint32_t *reg = mlxVirtToReg(map->getVirtualAddress(), offset);
    uint32_t value = *reg;
    OSSynchronizeIO();
    return OSSwapBigToHostInt32(value);
}

static inline void
mlxMMIOWrite32BE(IOMemoryMap *map, uintptr_t offset, uint32_t value)
{
    volatile uint32_t *reg = mlxVirtToReg(map->getVirtualAddress(), offset);
    *reg = OSSwapHostToBigInt32(value);
    OSSynchronizeIO();
}

#endif /* MLX_KERNEL_COMPAT_HPP */
