/*
 * MlxCmd.hpp — Firmware command interface (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/net/ethernet/mellanox/mlx5/core/cmd.c
 * Pure MMIO/DMA operations, no dependency on the Linux kernel, directly portable.
 */
#ifndef MLX_CMD_HPP
#define MLX_CMD_HPP

#include <libkern/OSTypes.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include "MlxRegs.hpp"
#include "MlxWQE.hpp"

/* Maximum number of command slots (See MLX5_MAX_COMMANDS) */
#define MLX_MAX_COMMANDS        (1 << 8)

/* Command descriptor size (64 bytes), See device.h:525 mlx5_cmd_layout */
#define MLX_COMMAND_DESCRIPTOR_SIZE 64

/* Mailbox data block size (512 bytes), See MLX5_CMD_DATA_BLOCK_SIZE */
#define MLX_CMD_DATA_BLOCK_SIZE 512

class MlxPCIDriver;

/*
 * Command descriptor layout — See device.h:525 struct mlx5_cmd_layout
 * Hard-imposed by hardware, must not be changed
 */
struct MlxCmdLayout {
    uint8_t  type;             /* +0  type */
    uint8_t  rsvd0[3];         /* +1 */
    uint32_t inlen;            /* +4  in data length */
    uint64_t in_ptr;           /* +8  in mailbox pointer (used for large inputs) */
    uint32_t in[4];            /* +16 command header (first 16 bytes: opcode/op_mod...) */
    uint32_t out[4];           /* +32 response header (first 16 bytes) */
    uint64_t out_ptr;          /* +48 out mailbox pointer */
    uint32_t outlen;           /* +56 out data length */
    uint8_t  token;            /* +60 token */
    uint8_t  sig;              /* +61 signature */
    uint8_t  rsvd1;            /* +62 */
    uint8_t  status_own;       /* +63 [7:1]status [0]ownership */
};

/*
 * Mailbox block (512B data + descriptor) — See mlx5_cmd_prot_block (device.h:781)
 */
struct MlxCmdMailbox {
    uint8_t  data[MLX_CMD_DATA_BLOCK_SIZE];   /* 512B */
    uint8_t  rsvd0[48];
    uint64_t next;              /* pointer to next block */
    uint32_t block_num;
    uint8_t  rsvd1;
    uint8_t  token;
    uint8_t  ctrl_sig;
    uint8_t  sig;
} __attribute__((packed));

/*
 * Command input/output descriptor
 */
struct MlxCmdInOut {
    const void *in;         /* command data (first 16 bytes are opcode/op_mod) */
    uint32_t    inSize;
    void       *out;        /* response buffer */
    uint32_t    outSize;
    uint32_t    opcode;     /* for debugging */
};

/*
 * Command entity (in-flight state of one command slot, See mlx5_cmd_work_ent)
 */
struct MlxCmdEnt {
    uint32_t    idx;            /* command slot index */
    uint8_t     token;
    bool        pending;
    bool        done;
    IOReturn    status;
    /* descriptor pointer (within command queue page) */
    MlxCmdLayout *lay;
    /* in/out mailbox (allocated for large commands) */
    IOMemoryDescriptor *inMailboxDesc;
    IOMemoryDescriptor *outMailboxDesc;
    void       *inMailbox;
    uint64_t    inMailboxDMA;
    void       *outMailbox;
    uint64_t    outMailboxDMA;
};

/*
 * Command interface class
 */
class MlxCmd : public OSObject {
    OSDeclareDefaultStructors(MlxCmd)

public:
    /* Init: allocate command queue page, validate cmdif_rev, program firmware */
    bool init(MlxPCIDriver *owner, IOMemoryMap *bar0, uint32_t cmdqSize);

    /* Execute one command synchronously (blocking path of mlx5_cmd_exec) */
    kern_return_t exec(MlxCmdInOut *cmd, uint32_t timeoutMs);

    /* Completion callback for event mode (called by MlxEQ) */
    void handleCompletion(uint32_t vector);

    /* Command interface state */
    bool isUp() const { return fUp; }
    uint16_t cmdifRev() const { return fCmdifRev; }

private:
    /* Allocate/free an idle slot */
    bool allocIdx(uint32_t *idx);
    void freeIdx(uint32_t idx);

    /* Submit command to the command queue page + doorbell */
    kern_return_t submit(uint32_t idx, MlxCmdInOut *cmd);

    /* Allocate/free mailbox (large command input >16B or output >16B) */
    kern_return_t allocMailbox(MlxCmdEnt *ent, uint32_t inSize, uint32_t outSize);
    void freeMailbox(MlxCmdEnt *ent);

    /* Signature (XOR checksum, See set_signature cmd.c:228) */
    void setSignature(MlxCmdLayout *lay);
    void setMailboxSignature(MlxCmdMailbox *mb, size_t len);

    /* Memory barrier */
    static void memoryBarrier() { OSMemoryBarrier(); }

    MlxPCIDriver    *fOwner;
    IOMemoryMap     *fBar0;
    IOMemoryDescriptor *fCmdBufDesc;
    void            *fCmdBuf;       /* command queue page virtual address */
    uint64_t         fCmdDMA;       /* command queue DMA address */
    uint8_t          fLogSz;
    uint8_t          fLogStride;
    uint32_t         fMaxRegCmds;
    uint32_t         fBitmask;      /* idle slot bitmap */
    uint8_t          fToken;
    IOSimpleLock    *fAllocLock;
    IOSimpleLock    *fTokenLock;
    MlxCmdEnt       *fEntArr[MLX_MAX_COMMANDS];
    bool             fUp;
    bool             fModeEvents;
    uint16_t         fCmdifRev;    /* command interface revision (filled by init) */
};

#endif /* MLX_CMD_HPP */
