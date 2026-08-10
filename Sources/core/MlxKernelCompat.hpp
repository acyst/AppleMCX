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
#include <IOKit/IODMACommand.h>

/* Order regular memory accesses before/after DMA ownership transitions. */
static inline void
mlxMemoryBarrier()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* mlx5 IFC offsets count bits from the most significant bit of byte zero. */
static inline void
mlxSetBits(void *buffer, uint32_t bitOffset, uint32_t bitWidth, uint64_t value)
{
    uint8_t *bytes = static_cast<uint8_t *>(buffer);
    for (uint32_t i = 0; i < bitWidth; i++) {
        uint32_t dstBit = bitOffset + i;
        uint8_t mask = static_cast<uint8_t>(1u << (7 - (dstBit & 7)));
        uint64_t srcMask = 1ULL << (bitWidth - i - 1);
        if (value & srcMask)
            bytes[dstBit >> 3] |= mask;
        else
            bytes[dstBit >> 3] &= static_cast<uint8_t>(~mask);
    }
}

static inline uint64_t
mlxGetBits(const void *buffer, uint32_t bitOffset, uint32_t bitWidth)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    uint64_t value = 0;
    for (uint32_t i = 0; i < bitWidth; i++) {
        uint32_t srcBit = bitOffset + i;
        value <<= 1;
        value |= (bytes[srcBit >> 3] >> (7 - (srcBit & 7))) & 1;
    }
    return value;
}

/* Keep the IOMMU mapping alive for the entire period of device ownership. */
static inline IOReturn
mlxMapDMAContiguous(IOMemoryDescriptor *memory, IODMACommand **mapping,
                    uint64_t *deviceAddress)
{
    if (!memory || !mapping || !deviceAddress || memory->getLength() == 0)
        return kIOReturnBadArgument;

    IODMACommand *command = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped,
        memory->getLength(), 1);
    if (!command)
        return kIOReturnNoMemory;

    IOReturn kr = command->setMemoryDescriptor(memory);
    if (kr != kIOReturnSuccess) {
        command->release();
        return kr;
    }

    UInt64 offset = 0;
    IODMACommand::Segment64 segment = {};
    UInt32 count = 1;
    kr = command->gen64IOVMSegments(&offset, &segment, &count);
    if (kr != kIOReturnSuccess || count != 1 ||
        segment.fLength < memory->getLength()) {
        command->clearMemoryDescriptor();
        command->release();
        return kIOReturnNoSpace;
    }

    *mapping = command;
    *deviceAddress = segment.fIOVMAddr;
    return kIOReturnSuccess;
}

static inline void
mlxUnmapDMA(IODMACommand *mapping)
{
    if (!mapping)
        return;
    mapping->clearMemoryDescriptor();
    mapping->release();
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
