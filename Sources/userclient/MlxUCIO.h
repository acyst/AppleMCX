/*
 * MlxUCIO.h — userspace interface definitions (shared by the driver and libmlx)
 *
 * Purpose:
 *   1. Kernel side: MlxUserClient::externalMethod dispatch table
 *   2. Userspace: libmlx's IOConnectCallMethod selectors
 *
 * Note: this file is included by both the kernel and userspace, so only POD structs are
 *       allowed; it must not depend on any kernel- or userspace-specific headers.
 */
#ifndef MLX_UC_IO_H
#define MLX_UC_IO_H

#include <stdint.h>
#include <stddef.h>

/* class ID: used with IOServiceOpen */
#define MLX_USERCLIENT_CLASS   "MlxUserClient"

/* memory mapping index (IOConnectMapMemory) */
enum {
    kMlxUCMemIndexUar        = 0,   /* UAR page (doorbell/BF) */
    kMlxUCMemIndexDbRecord   = 1,   /* DB record page */
    kMlxUCMemIndexCqe        = 2,   /* CQ buffer (optional) */
};

struct mlx_create_cq_req {
    uint32_t entries;
};

/* createCQ response; the CQ buffer is mapped through clientMemoryForType. */
struct mlx_create_cq_resp {
    uint32_t  cqHandle;
    uint32_t  logSize;          /* log of depth */
    uint32_t  cqeSize;
    uint32_t  dbRecordOffset;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_create_cq_req) == 4,
              "mlx_create_cq_req ABI mismatch");
static_assert(sizeof(struct mlx_create_cq_resp) == 16,
              "mlx_create_cq_resp ABI mismatch");
#endif

/* externalMethod selector */
enum {
    /* device */
    kMlxUCMethodOpen          = 0x1000,
    kMlxUCMethodClose         = 0x1001,
    kMlxUCMethodQueryDevice   = 0x1002,  /* capability query */
    kMlxUCMethodQueryPort     = 0x1003,  /* port state */

    /* PD / UAR */
    kMlxUCMethodAllocPD       = 0x1010,
    kMlxUCMethodDeallocPD     = 0x1011,
    kMlxUCMethodAllocUAR      = 0x1012,

    /* QP */
    kMlxUCMethodCreateQP      = 0x1020,
    kMlxUCMethodModifyQP      = 0x1021,
    kMlxUCMethodDestroyQP     = 0x1022,
    kMlxUCMethodQueryQP       = 0x1023,

    /* CQ */
    kMlxUCMethodCreateCQ      = 0x1030,
    kMlxUCMethodDestroyCQ     = 0x1031,

    /* MR */
    kMlxUCMethodRegMR         = 0x1040,
    kMlxUCMethodDeregMR       = 0x1041,

    /* AH */
    kMlxUCMethodCreateAH      = 0x1050,
    kMlxUCMethodDestroyAH     = 0x1051,

    /* GID */
    kMlxUCMethodGetGidIndex   = 0x1060,

    /* congestion control */
    kMlxUCMethodCCQuery       = 0x1070,
    kMlxUCMethodCCModify      = 0x1071,

    /* ===== firmware management (used by mlxconfig/mlxup/mlxlink) ===== */
    kMlxUCMethodAccessReg     = 0x1080,   /* ACCESS_REG register read/write */
    kMlxUCMethodFwCmd         = 0x1081,   /* firmware command passthrough (used by mlxup) */
    kMlxUCMethodQueryPages    = 0x1082,   /* QUERY_PAGES (firmware page management) */
    kMlxUCMethodPortStats     = 0x1083,   /* port statistics (used by mlxlink) */
    kMlxUCMethodFwReset       = 0x1084,   /* firmware reset (used by mlxfwreset) */
    kMlxUCMethodQueryFwVer    = 0x1085,   /* firmware version query */
    kMlxUCMethodQueryHealth   = 0x1086,   /* health status */

    /* DMA data path */
    kMlxUCMethodVirtToPhys    = 0x1090,   /* virtual address → physical address (used by post_send) */
    kMlxUCMethodGetCqBuffer   = 0x1091,   /* get the CQ buffer descriptor (used by poll_cq) */

    /* completion events */
    kMlxUCMethodQueryCqCompletions = 0x1092,  /* query the CQ completion count */

    /* async events */
    kMlxUCMethodGetAsyncEvent = 0x1093,  /* get an async event (non-blocking) */

    /* CQ consumer index update (kernel-mediated, replaces direct DB record write) */
    kMlxUCMethodUpdateCqConsumer = 0x1094,
};

/* updateCqConsumer request: tell the kernel the new consumer index for a CQ */
struct mlx_update_cq_consumer_req {
    uint32_t  cqHandle;
    uint32_t  consumerIndex;
};

/* async event (see rdma-core ibv_async_event) */
struct mlx_async_event {
    uint32_t  eventType;       /* see ibv_event_type */
    uint32_t  elementType;     /* MLX_ASYNC_ELEMENT_*: 0=device 1=CQ 2=QP 3=port */
    uint32_t  elementHandle;   /* CQ/QP handle or port_num */
    uint32_t  reserved;
};

/* element types an async event can belong to */
enum {
    MLX_ASYNC_ELEMENT_DEVICE = 0,
    MLX_ASYNC_ELEMENT_CQ     = 1,
    MLX_ASYNC_ELEMENT_QP     = 2,
    MLX_ASYNC_ELEMENT_PORT   = 3,
};

/* event types (see ibv_event_type) */
enum {
    MLX_EVENT_CQ_ERR = 0,
    MLX_EVENT_QP_FATAL = 1,
    MLX_EVENT_COMM_EST = 4,
    MLX_EVENT_SQ_DRAINED = 5,
    MLX_EVENT_PATH_MIG = 6,
    MLX_EVENT_DEVICE_FATAL = 8,
    MLX_EVENT_PORT_ACTIVE = 9,
    MLX_EVENT_PORT_ERR = 10,
    MLX_EVENT_GID_CHANGE = 18,
    MLX_EVENT_WQ_FATAL = 19,
};

/* ===== firmware management structs (POD) ===== */

/* ACCESS_REG request: register_id + read/write direction + data */
struct mlx_access_reg_req {
    uint32_t  registerId;      /* e.g. PFCC/QTCT/PVLC, etc. */
    uint32_t  opMod;           /* 0=read 1=write */
    uint32_t  argument;        /* additional argument */
    uint8_t   data[256];       /* register data */
    uint32_t  dataSize;
};
struct mlx_access_reg_resp {
    uint8_t   data[256];
    uint32_t  dataSize;
};

/* firmware command passthrough (used by mlxup) */
struct mlx_fw_cmd_req {
    uint16_t  opcode;          /* firmware command opcode */
    uint16_t  opMod;
    uint8_t   in[512];
    uint32_t  inSize;
};
struct mlx_fw_cmd_resp {
    uint8_t   out[512];
    uint32_t  outSize;
};

/* firmware version */
struct mlx_fw_ver_resp {
    uint32_t  fwRev;           /* version number (encoded) */
    uint32_t  cmdifRev;
    uint16_t  deviceId;
    uint8_t   portType;
    uint32_t  numPorts;
};

/* port statistics (used by mlxlink) */
struct mlx_port_stats_resp {
    uint64_t  rxPkts;
    uint64_t  txPkts;
    uint64_t  rxBytes;
    uint64_t  txBytes;
    uint64_t  rxDrop;
    uint64_t  txDrop;
    uint64_t  rxErrors;
    uint64_t  txErrors;
    uint32_t  linkSpeed;
    uint8_t   linkState;       /* 0=down 1=up */
    uint8_t   portNum;
};

/* ========== struct definitions (POD) ========== */

/* device capability query response */
struct mlx_query_device_resp {
    uint64_t  fwVersion;
    uint32_t  deviceId;
    uint32_t  numPorts;
    uint32_t  maxQp;
    uint32_t  maxCq;
    uint32_t  maxMr;
    uint16_t  roceVersions;     /* bit0=RoCEv1 bit1=RoCEv2 */
    uint16_t  maxGid;
    uint32_t  maxMsgSize;
};

/* port attributes response */
/* link layer types (see rdma_link_layer, used for mlx_query_port_resp.linkLayer) */
enum {
    MLX_LINK_LAYER_UNSPECIFIED = 0,
    MLX_LINK_LAYER_INFINIBAND  = 1,
    MLX_LINK_LAYER_ETHERNET    = 2,
};

struct mlx_query_port_resp {
    uint32_t  portNum;
    uint8_t   linkLayer;        /* see MlxLinkLayer: 1=IB 2=Ethernet */
    uint8_t   portState;        /* 0=down 1=up */
    uint8_t   gidType;          /* 2=RoCEv2 */
    uint8_t   rsvd;
    uint32_t  activeSpeed;      /* Mbps */
    uint32_t  maxMtu;
    /* IB attributes (reserved for Option C, see ib_port_attr) */
    uint16_t  lid;              /* local LID */
    uint16_t  smLid;            /* subnet manager LID */
    uint16_t  pkeyTblLen;       /* P_Key table length */
    uint16_t  gidTblLen;        /* GID table length */
};

/* createQP request/response */
struct mlx_create_qp_req {
    uint32_t  pd;
    uint32_t  sendCq;
    uint32_t  recvCq;
    uint32_t  qpType;           /* 0=RC 1=UD */
    uint32_t  sqSize;           /* power of 2 */
    uint32_t  rqSize;
    uint64_t  sqBufAddr;        /* user SQ buffer (virtual address) */
    uint64_t  rqBufAddr;        /* user RQ buffer (virtual address) */
    uint32_t  dbRecordOffset;   /* DB record user offset */
    uint32_t  bfOffset;         /* BF doorbell user offset */
    uint32_t  maxInlineData;
    uint32_t  rsvd;
};
struct mlx_create_qp_resp {
    uint32_t  qpn;
    uint32_t  sqStrideSize;     /* WQE stride after alignment */
    uint32_t  dbRecordOffset;   /* RQ/SQ DB record pair in mapped DB page */
    uint32_t  bfOffset;         /* BF register offset in mapped UAR */
    uint64_t  rsvd[2];
};

/* modifyQP (state machine) */
struct mlx_modify_qp_req {
    uint32_t  qpn;
    uint32_t  curState;         /* 0=RST 1=INIT 2=RTR 3=RTS */
    uint32_t  newState;
    uint32_t  attrMask;         /* compatible with the Linux ib_qp_attr_mask */
    uint32_t  destQpn;
    uint32_t  pathMtu;
    uint32_t  rqPsn;
    uint32_t  sqPsn;
    uint32_t  pkeyIndex;
    uint32_t  portNum;
    /* AH (used to encode the path on RTR) */
    uint8_t   ahDmac[6];
    uint8_t   ahDgid[16];       /* destination IP */
    uint32_t  ahSgidIndex;      /* source GID */
    uint8_t   ahHopLimit;
    uint8_t   ahTrafficClass;   /* DSCP */
    uint16_t  ahUdpSport;
    uint32_t  minRnrTimer;
    uint32_t  maxDestRdAtomic;
    uint32_t  maxRdAtomic;
    uint32_t  rsvd;
};

/* regMR request/response */
struct mlx_reg_mr_req {
    uint64_t  startAddr;
    uint64_t  length;
    uint32_t  accessFlags;      /* bit0=LOCAL_WRITE bit1=REMOTE_WRITE ... */
    uint32_t  pd;
};
struct mlx_reg_mr_resp {
    uint32_t  mrHandle;
    uint32_t  lkey;
    uint32_t  rkey;
    uint32_t  rsvd;
};

/* createAH */
struct mlx_create_ah_req {
    uint8_t   dmac[6];
    uint8_t   dgid[16];
    uint32_t  sgidIndex;
    uint8_t   hopLimit;
    uint8_t   trafficClass;
    uint16_t  udpSport;
    uint32_t  portNum;
    /* IB addressing (Option C: see ah.c:97-98)
     * ahType: 0=RoCE 1=IB */
    uint32_t  ahType;
    uint16_t  dlid;             /* destination LID (IB) */
    uint8_t   pathBits;         /* path bits (IB) */
    uint8_t   sl;               /* service level (IB) */
};
struct mlx_create_ah_resp {
    uint32_t  ahHandle;
    uint32_t  rsvd;
};

/* congestion control */
struct mlx_cc_params {
    uint32_t  rpgMinDecFac;     /* multiplicative decrease factor */
    uint32_t  rpgAiRate;        /* additive increase rate */
    uint32_t  rpgTimeReset;     /* increase timer */
    uint32_t  rpgThreshold;     /* ECN marking threshold */
    uint32_t  rpgHai;
    uint32_t  rpgGd;
    uint32_t  rpgTimeInc;
    uint32_t  rsvd;
};

#endif /* MLX_UC_IO_H */
