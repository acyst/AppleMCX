/*
 * MlxRegs.hpp — Hardware register layouts (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/include/linux/mlx5/device.h
 * These layouts are hard-imposed by hardware, consistent across
 * ConnectX-4 ~ ConnectX-8, and must not be changed.
 *
 * Framework note: this driver is a generic adapter framework covering the
 * entire mlx5 family; mcx5 (ConnectX-5, DID 0x1017) is the first implementation target.
 */
#ifndef MLX_REGS_HPP
#define MLX_REGS_HPP

#include <stdint.h>
#include <stdbool.h>

/* libkern compatibility (also visible outside the kernel environment) */
#ifndef APPLE_KEXT_OVERRIDE
#define APPLE_KEXT_OVERRIDE
#endif

/*
 * Init Segment — BAR0 base address
 * See: device.h:568 struct mlx5_init_seg
 */
struct MlxInitSeg {
    uint32_t fw_rev;               /* +0x0000 firmware version */
    uint32_t cmdif_rev_fw_sub;     /* +0x0004 command interface version */
    uint32_t rsvd0[2];             /* +0x0008 */
    uint32_t cmdq_addr_h;          /* +0x0010 command queue DMA high 32 bits */
    uint32_t cmdq_addr_l_sz;       /* +0x0014 low 12 bits: log_sz/log_stride */
    uint32_t cmd_dbell;            /* +0x0018 command doorbell ★ */
    uint32_t rsvd1[120];           /* +0x001C */
    uint32_t initializing;         /* +0x0200 bit31 = firmware initializing */
    struct MlxHealthBuffer {
        uint32_t hw_health_counter; /* +0x0204 */
        uint32_t fw_health_counter; /* +0x0208 */
        uint32_t health_flags;      /* +0x020C */
        uint32_t rsvd2[2];          /* +0x0210 */
        uint32_t synd;              /* +0x0218 health error code */
        uint32_t ext_synd;          /* +0x021C */
        uint32_t rsvd3[100];
    } health;
    uint32_t rsvd4[878];
    uint32_t cmd_exec_to;          /* command execution timeout */
};

/* Command interface version (See CMD_IF_REV) */
#define MLX_CMD_IF_REV          5

/* Command descriptor ownership bit */
#define MLX_CMD_OWNER_HW        (1u << 0)

/* Command descriptor size (bytes) */
#define MLX_COMMAND_DESCRIPTOR_SIZE 64

/*
 * Register doorbell offsets (relative to the UAR page)
 * See: include/linux/mlx5/doorbell.h
 */
#define MLX_BF_OFFSET           0x800   /* BF register ★ user-space doorbell */
#define MLX_CQ_DOORBELL         0x20    /* CQ doorbell */
#define MLX_EQ_DOORBELL         0x40    /* EQ doorbell */

/*
 * Command opcodes — independent of hardware version, shared across the whole family
 * See: mlx5_ifc.h:115-247
 */
enum {
    MLX_CMD_OP_QUERY_HCA_CAP          = 0x100,
    MLX_CMD_OP_SET_HCA_CAP            = 0x101,
    MLX_CMD_OP_ENABLE_HCA             = 0x103,
    MLX_CMD_OP_DISABLE_HCA            = 0x104,
    MLX_CMD_OP_QUERY_ISSI             = 0x105,
    MLX_CMD_OP_SET_ISSI               = 0x106,
    MLX_CMD_OP_INIT_HCA               = 0x102,
    MLX_CMD_OP_TEARDOWN_HCA           = 0x107,
    MLX_CMD_OP_MANAGE_PAGES           = 0x108,
    MLX_CMD_OP_QUERY_PAGES            = 0x109,
    MLX_CMD_OP_ALLOC_UAR              = 0x10B,
    MLX_CMD_OP_FREE_UAR               = 0x10C,
    MLX_CMD_OP_ACCESS_REG             = 0x805,
    MLX_CMD_OP_CREATE_MKEY            = 0x200,
    MLX_CMD_OP_DESTROY_MKEY           = 0x201,
    MLX_CMD_OP_QUERY_MKEY             = 0x202,
    MLX_CMD_OP_CREATE_EQ              = 0x300,
    MLX_CMD_OP_DESTROY_EQ             = 0x301,
    MLX_CMD_OP_QUERY_EQ               = 0x302,
    MLX_CMD_OP_CREATE_CQ              = 0x400,
    MLX_CMD_OP_DESTROY_CQ             = 0x401,
    MLX_CMD_OP_QUERY_CQ               = 0x402,
    MLX_CMD_OP_CREATE_QP              = 0x500,
    MLX_CMD_OP_DESTROY_QP             = 0x501,
    MLX_CMD_OP_QUERY_QP               = 0x502,
    MLX_CMD_OP_RST2INIT_QP            = 0x503,
    MLX_CMD_OP_INIT2RTR_QP            = 0x504,
    MLX_CMD_OP_RTR2RTS_QP             = 0x505,
    MLX_CMD_OP_RTS2RTS_QP             = 0x506,
    MLX_CMD_OP_SQERR2RTS_QP           = 0x507,
    MLX_CMD_OP_2RST_QP                = 0x50A,
    MLX_CMD_OP_2ERR_QP                = 0x50B,
    MLX_CMD_OP_SET_ROCE_ADDRESS       = 0x761,
    MLX_CMD_OP_QUERY_ROCE_ADDRESS     = 0x762,
    MLX_CMD_OP_MODIFY_CONG_PARAMS     = 0x825,
    MLX_CMD_OP_QUERY_CONG_PARAMS      = 0x826,
    MLX_CMD_OP_QUERY_CONG_STATUS      = 0x827,
    MLX_CMD_OP_QUERY_CONG_STATISTICS  = 0x828,
    /* mlx5e NIC data path (See mlx5_ifc.h command opcodes) */
    MLX_CMD_OP_ALLOC_PD                = 0x800,
    MLX_CMD_OP_DEALLOC_PD              = 0x801,
    MLX_CMD_OP_ALLOC_TRANSPORT_DOMAIN  = 0x816,
    MLX_CMD_OP_DEALLOC_TRANSPORT_DOMAIN = 0x817,
    MLX_CMD_OP_CREATE_TIR              = 0x900,
    MLX_CMD_OP_DESTROY_TIR             = 0x902,
    MLX_CMD_OP_CREATE_SQ               = 0x904,
    MLX_CMD_OP_DESTROY_SQ              = 0x906,
    MLX_CMD_OP_CREATE_RQ               = 0x908,
    MLX_CMD_OP_DESTROY_RQ              = 0x90a,
    MLX_CMD_OP_CREATE_TIS              = 0x912,
    MLX_CMD_OP_DESTROY_TIS             = 0x914,
    MLX_CMD_OP_CREATE_RQT              = 0x916,
    MLX_CMD_OP_DESTROY_RQT             = 0x918,
};

/*
 * HCA capability field access macros (generic mlx5 family)
 * See: MLX5_CAP_GEN / MLX5_CAP_ROCE (device.h)
 * Capabilities are in units of 64 dwords (256 bytes), returned by QUERY_HCA_CAP
 */
#define MLX_CAP_FIELD(capBytes, field)  /* provided by the MLX_SET/MLX_GET generator */

/*
 * RoCE versions (See device.h:402)
 */
enum {
    MLX_ROCE_VERSION_1 = 0,      /* RoCE v1: EtherType 0x8915 */
    MLX_ROCE_VERSION_2 = 2,      /* RoCE v2: UDP 4791 */
};

/*
 * RoCE v2 ports (See drivers/infiniband/core/lag.c:39)
 */
#define MLX_ROCE_V2_UDP_DPORT    4791
#define MLX_ROCE_V2_CNP_DPORT    4792   /* CNP = dport + 1 */

/*
 * EQ event types (See device.h:354)
 */
enum {
    MLX_EVENT_TYPE_COMPLETION     = 0x00,
    MLX_EVENT_TYPE_PATH_MIG       = 0x01,
    MLX_EVENT_TYPE_COMM_EST       = 0x02,
    MLX_EVENT_TYPE_SQ_DRAINED     = 0x03,
    MLX_EVENT_TYPE_WQ_CATAS_ERROR = 0x06,
    MLX_EVENT_TYPE_CMD            = 0x0a,   /* command completion */
    MLX_EVENT_TYPE_PAGE_REQUEST   = 0x0b,   /* firmware requests pages */
    MLX_EVENT_TYPE_SRQ_LAST_WQE   = 0x13,
    MLX_EVENT_TYPE_SRQ_RQ_LIMIT   = 0x14,
    MLX_EVENT_TYPE_NIC_VPORT_CHANGE = 0x1c,
    /* port/device-level events (See Linux device.h:354 mlx5_event) */
    MLX_EVENT_TYPE_DEVICE_FATAL      = 0x22,
    MLX_EVENT_TYPE_PORT_STATE_CHANGE = 0x09,
    MLX_EVENT_TYPE_CLIENT_REREGISTER = 0x30,
    MLX_EVENT_TYPE_GID_CHANGE        = 0x08,
};

/*
 * Transport type st field (QPC)
 */
enum {
    MLX_QP_ST_RC   = 0x0,
    MLX_QP_ST_UD   = 0x1,
    MLX_QP_ST_UC   = 0x3,
    MLX_QP_ST_XRC  = 0x6,
};

/*
 * QP states (See qp.c:858 to_mlx5_state)
 */
enum {
    MLX_QP_STATE_RST   = 0,
    MLX_QP_STATE_INIT  = 1,
    MLX_QP_STATE_RTR   = 2,
    MLX_QP_STATE_RTS   = 3,
    MLX_QP_STATE_SQER  = 4,
    MLX_QP_STATE_SQD   = 5,
    MLX_QP_STATE_ERR   = 6,
};

#endif /* MLX_REGS_HPP */
