/*
 * MlxWQE.hpp — WQE/CQE/AV hardware structures (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/include/linux/mlx5/qp.h
 *              kernel_src/mlnx-ofed-kernel-5.9/include/linux/mlx5/device.h
 * The layouts are hard-imposed by hardware, consistent across the whole family,
 * and must not be changed. Use static_assert to validate sizes.
 */
#ifndef MLX_WQE_HPP
#define MLX_WQE_HPP

#include <stdint.h>

/* 32/64-bit network byte-order macros (arm64 little-endian, for reading hardware fields) */
#define MLX_BE32(val)  __builtin_bswap32((uint32_t)(val))
#define MLX_BE16(val)  __builtin_bswap16((uint16_t)(val))

/*
 * WQE control segment (16 bytes) — at the start of every WQE
 * See: qp.h:204 struct mlx5_wqe_ctrl_seg
 */
struct MlxWqeCtrlSeg {
    uint32_t opmod_idx_opcode;  /* [31:24]opmod [23:8]WQE index [7:0]opcode */
    uint32_t qpn_ds;            /* [15:0]QPN [31:16]WQE size (16B units) */
    uint8_t  signature;         /* XOR checksum */
    uint8_t  rsvd[2];
    uint8_t  fm_ce_se;          /* [3]fm [2]ce [1]se */
    union {
        uint32_t general_id;    /* [31:16]general_id [15:0]imm */
        uint32_t imm;
        uint32_t umr_mkey;
        uint32_t tis_tir_num;
    };
};

/* ctrl segment fm_ce_se bits */
#define MLX_WQE_CTRL_CQ_UPDATE  0x2   /* ce */
#define MLX_WQE_CTRL_SOLICIT    0x1   /* se */

/*
 * WQE data segment (16 bytes) — one per SGE
 * See: qp.h:376 struct mlx5_wqe_data_seg
 */
struct MlxWqeDataSeg {
    uint32_t byte_count;
    uint32_t lkey;
    uint64_t addr;
};

/*
 * WQE remote address segment (remote address) — RDMA WRITE/READ
 * See: qp.h:372 struct mlx5_wqe_raddr_seg
 */
struct MlxWqeRaddrSeg {
    uint64_t raddr;
    uint32_t rkey;
    uint32_t rsvd;
};

/*
 * WQE datagram segment — for UD, embeds mlx5_av
 * See: qp.h:388 struct mlx5_wqe_datagram_seg
 */
struct MlxWqeDatagramSeg {
    uint32_t dqp_dct;       /* destination QPN | MLX5_EXTENDED_UD_AV */
    uint32_t av;
};

/*
 * WQE Ethernet segment (16 bytes) — for Ethernet TX
 * See: qp.h:276 struct mlx5_wqe_eth_seg
 */
struct MlxWqeEthSeg {
    uint8_t  swp_outer_l4_offset;
    uint8_t  swp_outer_l3_offset;
    uint8_t  swp_inner_l4_offset;
    uint8_t  swp_inner_l3_offset;
    uint8_t  cs_flags;          /* checksum offload flags */
    uint8_t  swp_flags;
    uint16_t mss;               /* GSO segmentation size */
    uint32_t flow_table_metadata;
    union {
        struct {
            uint16_t sz;        /* inline header size */
            uint8_t  start[2];
        } inline_hdr;
        struct {
            uint16_t type;
            uint16_t vlan_tci;
        } insert;
        uint32_t trailer;
    };
};

/* cs_flags bits (See en_tx.c) */
#define MLX_ETH_WQE_L3_CSUM     0x01
#define MLX_ETH_WQE_L4_CSUM     0x02
#define MLX_ETH_WQE_L3_INNER_CSUM 0x08
#define MLX_ETH_WQE_L4_INNER_CSUM 0x10

/*
 * WQE inline segment — embedded data (small-packet optimization)
 * See: qp.h:439 struct mlx5_wqe_inline_seg
 */
struct MlxWqeInlineSeg {
    uint32_t byte_count;
    uint32_t data[];            /* embedded data */
};

/* Complete Ethernet TX WQE layout (See en.h:244 mlx5e_tx_wqe) */
struct MlxEthTxWqe {
    struct MlxWqeCtrlSeg ctrl;
    struct MlxWqeEthSeg  eth;
    struct MlxWqeDataSeg data[];
};

/*
 * Address Vector AV (28 bytes) — the core of RoCE addressing
 * See: qp.h:327 struct mlx5_av (exact replica, including qkey.reserved and reserved0)
 */
struct MlxAV {
    union {
        struct {
            uint32_t qkey;      /* UD QKEY */
            uint32_t reserved;
        } qkey;
        uint64_t dc_key;       /* for DC */
    } key;                     /* 8 bytes */
    uint32_t dqp_dct;          /* destination QPN (UD) */
    uint8_t  stat_rate_sl;     /* [7:4]static_rate [3:0]SL (Ethernet priority) */
    uint8_t  fl_mlid;          /* IB path bits */
    union {
        uint16_t rlid;         /* IB DLID */
        uint16_t udp_sport;    /* RoCEv2 source UDP port ★ */
    };                         /* 2 bytes */
    uint8_t  reserved0[4];     /* alignment padding */
    uint8_t  rmac[6];          /* destination MAC ★ */
    uint8_t  tclass;           /* DSCP | ECN bits ★ */
    uint8_t  hop_limit;        /* TTL ★ */
    uint32_t grh_gid_fl;       /* [30]=GRH present [29:20]=sgid_index [19:0]=flow_label */
    uint8_t  rgid[16];         /* destination GID (remote IP) ★ */
};

/*
 * grh_gid_fl bit definitions (See ah.c:63)
 *   grh_gid_fl = flow_label | (1 << 30) | sgid_index << 20
 *   bit30  = GRH present flag
 *   bits 29:20 = sgid_index (index into the source GID table)
 *   bits 19:0  = flow_label
 */
#define MLX_AV_GRH_PRESENT     (1u << 30)
#define MLX_AV_SGID_INDEX_SHIFT 20
#define MLX_AV_FLOW_LABEL_MASK  0xFFFFF

/* ECN bit (See ah.c:94 MLX5_ECN_ENABLED) */
#define MLX_AV_ECN_ENABLED     (1u << 1)

/*
 * CQE (64 bytes) — Completion Queue Element
 * See: device.h:808 struct mlx5_cqe64 (exact replica of all fields)
 */
struct MlxCqe64 {
    uint8_t  tls_outer_l3_tunneled;
    uint8_t  rsvd0;
    uint16_t wqe_id;
    union {
        struct {
            uint8_t  tcppsh_abort_dupack;
            uint8_t  min_ttl;
            uint16_t tcp_win;
            uint32_t ack_seq_num;
        } lro;
        struct {
            uint8_t  reserved0;
            uint8_t  header_size;
            uint16_t header_entry_index;
            uint32_t data_offset;
        } shampo;
    };
    uint32_t rss_hash_result;
    uint8_t  rss_hash_type;
    uint8_t  ml_path;
    uint8_t  rsvd20[2];
    uint16_t check_sum;
    uint16_t slid;              /* 0 under RoCE */
    uint32_t flags_rqpn;        /* low 24 bits RQPN */
    uint8_t  hds_ip_ext;
    uint8_t  l4_l3_hdr_type;
    uint16_t vlan_info;         /* RoCE VLAN */
    uint32_t srqn;              /* [31:24]lro_num_seg [23:0]srqn */
    uint32_t imm_inval_pkey;    /* immediate / inval_rkey / pkey */
    uint8_t  rsvd40[4];
    uint32_t byte_cnt;          /* data length */
    uint32_t timestamp_h;
    uint32_t timestamp_l;
    uint32_t sop_drop_qpn;      /* [31:28]sop [27:24]drop [23:0]QPN */
    uint16_t wqe_counter;
    union {
        uint8_t  signature;
        uint8_t  validity_iteration_count;
    };
    uint8_t  op_own;            /* ★ [7:4]opcode [1:0]owner */
};

/* CQE ownership bit: (op_own >> 1) & 1 */
#define MLX_CQE_OWNER_MASK      (1u << 1)

/* CQE opcode bits: op_own >> 4 */
#define MLX_CQE_GET_OPCODE(cqe) ((cqe)->op_own >> 4)

enum {
    MLX_CQE_REQ      = 0,
    MLX_CQE_RESP_ERR = 1,
    MLX_CQE_RESP     = 2,
    MLX_CQE_REQ_ERR  = 4,
    MLX_CQE_SIG_ERR  = 8,
};

/* Compute static checks
 * Note: AV is 48 bytes (the original driver is not packed, includes alignment padding) */
static_assert(sizeof(struct MlxWqeCtrlSeg) == 16, "ctrl seg must be 16 bytes");
static_assert(sizeof(struct MlxWqeDataSeg) == 16,  "data seg must be 16 bytes");
static_assert(sizeof(struct MlxWqeRaddrSeg) == 16, "raddr seg must be 16 bytes");
static_assert(sizeof(struct MlxWqeEthSeg) == 16,   "eth seg must be 16 bytes");
static_assert(sizeof(struct MlxAV) == 48,          "AV must be 48 bytes");
static_assert(sizeof(struct MlxCqe64) == 64,       "CQE64 must be 64 bytes");

/*
 * WQE opcodes (See wr.c)
 */
enum {
    MLX_OPCODE_SEND             = 0x0A,
    MLX_OPCODE_SEND_IMM         = 0x0B,
    MLX_OPCODE_RDMA_WRITE       = 0x08,
    MLX_OPCODE_RDMA_WRITE_IMM   = 0x09,
    MLX_OPCODE_RDMA_READ        = 0x10,
    MLX_OPCODE_ATOMIC_CS        = 0x11,
    MLX_OPCODE_ATOMIC_FA        = 0x12,
    MLX_OPCODE_UMR              = 0x25,
    MLX_OPCODE_NOP              = 0x00,
    MLX_OPCODE_RECV             = 0x20,
};

#endif /* MLX_WQE_HPP */
