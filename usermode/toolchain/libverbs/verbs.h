/*
 * verbs.h — RDMA verbs API compatibility layer (macOS)
 *
 * Provides a subset of Linux rdma-core's verbs.h, internally mapped to
 * libmlx (MlxUserClient).
 * Goal: allow perftest and other tools to compile and run on macOS without
 * modification.
 *
 * Implementation principles:
 *   - Keep Linux verbs API signatures identical
 *   - Use libmlx's IOConnectCallMethod control path underneath, plus the
 *     zero-copy data path
 */
#ifndef VERBS_H
#define VERBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* libmlx forward declarations (full definitions provided by libmlx.h) */
struct mlx_qp;
struct mlx_context;

/* ---- Basic types ---- */
typedef uint32_t uint32_be_t;
typedef int32_t  int32_be_t;
typedef uint64_t uint64_be_t;
typedef uint16_t uint16_be_t;

enum { IBV_SYSFS_NAME_MAX = 64, IBV_SYSCONF_PATH_MAX = 256 };

/* Access flags */
enum {
    IBV_ACCESS_LOCAL_WRITE = 1,
    IBV_ACCESS_REMOTE_WRITE = (1 << 1),
    IBV_ACCESS_REMOTE_READ = (1 << 2),
    IBV_ACCESS_REMOTE_ATOMIC = (1 << 3),
    IBV_ACCESS_MW_BIND = (1 << 4),
    IBV_ACCESS_ZERO_BASED = (1 << 5),
    IBV_ACCESS_ON_DEMAND = (1 << 6),
    IBV_ACCESS_HUGETLB = (1 << 7),
};

/* QP type */
enum ibv_qp_type {
    IBV_QPT_RC = 2,
    IBV_QPT_UC = 3,
    IBV_QPT_UD = 1,
    IBV_QPT_RAW_PACKET = 8,
    IBV_QPT_XRC_INI = 9,
    IBV_QPT_XRC_TGT = 10,
};

/* QP state */
enum ibv_qp_state {
    IBV_QPS_RESET = 0,
    IBV_QPS_INIT = 1,
    IBV_QPS_RTR = 2,
    IBV_QPS_RTS = 3,
    IBV_QPS_SQD = 4,
    IBV_QPS_SQE = 5,
    IBV_QPS_ERR = 6,
    IBV_QPS_UNKNOWN = 7,
};

/* QP attribute mask */
enum {
    IBV_QP_STATE = 1,
    IBV_QP_CUR_STATE = (1 << 1),
    IBV_QP_EN_SQD_ASYNC_NOTIFY = (1 << 2),
    IBV_QP_ACCESS_FLAGS = (1 << 3),
    IBV_QP_PKEY_INDEX = (1 << 4),
    IBV_QP_PORT = (1 << 5),
    IBV_QP_QKEY = (1 << 6),
    IBV_QP_AV = (1 << 7),
    IBV_QP_PATH_MTU = (1 << 8),
    IBV_QP_TIMEOUT = (1 << 9),
    IBV_QP_RETRY_CNT = (1 << 10),
    IBV_QP_RNR_RETRY = (1 << 11),
    IBV_QP_RQ_PSN = (1 << 12),
    IBV_QP_MAX_QP_RD_ATOMIC = (1 << 13),
    IBV_QP_ALT_PATH = (1 << 14),
    IBV_QP_MIN_RNR_TIMER = (1 << 15),
    IBV_QP_SQ_PSN = (1 << 16),
    IBV_QP_MAX_DEST_RD_ATOMIC = (1 << 17),
    IBV_QP_PATH_MIG_STATE = (1 << 18),
    IBV_QP_CAP = (1 << 20),
    IBV_QP_DEST_QPN = (1 << 21),
};

/* MTU */
enum ibv_mtu {
    IBV_MTU_256 = 1,
    IBV_MTU_512 = 2,
    IBV_MTU_1024 = 3,
    IBV_MTU_2048 = 4,
    IBV_MTU_4096 = 5,
};

/* Link layer */
enum ibv_link_layer {
    IBV_LINK_LAYER_UNSPECIFIED,
    IBV_LINK_LAYER_INFINIBAND,
    IBV_LINK_LAYER_ETHERNET,
};

/* Port state */
enum ibv_port_state {
    IBV_PORT_NOP = 0,
    IBV_PORT_DOWN = 1,
    IBV_PORT_INIT = 2,
    IBV_PORT_ARMED = 3,
    IBV_PORT_ACTIVE = 4,
};

/* Path migration state */
enum ibv_mig_state {
    IBV_MIG_MIGRATED = 0,
    IBV_MIG_REARM = 1,
    IBV_MIG_ARMED = 2,
};

/* Async event type (see rdma-core verbs.h:449) */
enum ibv_event_type {
    IBV_EVENT_CQ_ERR,
    IBV_EVENT_QP_FATAL,
    IBV_EVENT_QP_REQ_ERR,
    IBV_EVENT_QP_ACCESS_ERR,
    IBV_EVENT_COMM_EST,
    IBV_EVENT_SQ_DRAINED,
    IBV_EVENT_PATH_MIG,
    IBV_EVENT_PATH_MIG_ERR,
    IBV_EVENT_DEVICE_FATAL,
    IBV_EVENT_PORT_ACTIVE,
    IBV_EVENT_PORT_ERR,
    IBV_EVENT_LID_CHANGE,
    IBV_EVENT_PKEY_CHANGE,
    IBV_EVENT_SM_CHANGE,
    IBV_EVENT_SRQ_ERR,
    IBV_EVENT_SRQ_LIMIT_REACHED,
    IBV_EVENT_QP_LAST_WQE_REACHED,
    IBV_EVENT_CLIENT_REREGISTER,
    IBV_EVENT_GID_CHANGE,
    IBV_EVENT_WQ_FATAL,
};

/* SRQ (MVP: minimal definition) */
struct ibv_srq {
    struct ibv_context *context;
    void *srq_context;
    struct ibv_pd *pd;
    uint32_t handle;
};

/* Async event (see verbs.h:472) */
struct ibv_async_event {
    union {
        struct ibv_cq  *cq;
        struct ibv_qp  *qp;
        struct ibv_srq *srq;
        int port_num;
    } element;
    enum ibv_event_type event_type;
};

/* CQ event (ibv_get_cq_event output) */
struct ibv_cq_event {
    struct ibv_cq *cq;
    void *cq_context;
};

/* Work request opcode */
enum ibv_wr_opcode {
    IBV_WR_RDMA_WRITE,
    IBV_WR_RDMA_WRITE_WITH_IMM,
    IBV_WR_SEND,
    IBV_WR_SEND_WITH_IMM,
    IBV_WR_RDMA_READ,
    IBV_WR_ATOMIC_CMP_AND_SWP,
    IBV_WR_ATOMIC_FETCH_AND_ADD,
    IBV_WR_LOCAL_INV,
    IBV_WR_BIND_MW,
    IBV_WR_SEND_WITH_INV,
    IBV_WR_TSO,
};

/* Completion status */
enum ibv_wc_status {
    IBV_WC_SUCCESS,
    IBV_WC_LOC_LEN_ERR,
    IBV_WC_LOC_QP_OP_ERR,
    IBV_WC_LOC_EEC_OP_ERR,
    IBV_WC_LOC_PROT_ERR,
    IBV_WC_WR_FLUSH_ERR,
    IBV_WC_MW_BIND_ERR,
    IBV_WC_BAD_RESP_ERR,
    IBV_WC_LOC_ACCESS_ERR,
    IBV_WC_REM_INV_REQ_ERR,
    IBV_WC_REM_ACCESS_ERR,
    IBV_WC_REM_OP_ERR,
    IBV_WC_RETRY_EXC_ERR,
    IBV_WC_RNR_RETRY_EXC_ERR,
    IBV_WC_LOC_RDD_VIOL_ERR,
    IBV_WC_REM_INV_RD_REQ_ERR,
    IBV_WC_REM_ABORT_ERR,
    IBV_WC_INV_EECN_ERR,
    IBV_WC_INV_EEC_STATE_ERR,
    IBV_WC_FATAL_ERR,
    IBV_WC_RESP_TIMEOUT_ERR,
    IBV_WC_GENERAL_ERR,
};

/* ---- Handle types ---- */
struct ibv_context;
struct ibv_device;
struct ibv_device_attr;
struct ibv_port_attr;
struct ibv_sge;
struct ibv_send_wr;
struct ibv_recv_wr;
struct ibv_wc;
struct ibv_qp_attr;
struct ibv_global_route;
struct ibv_grh;
struct ibv_flow;
struct ibv_srq;
struct ibv_ah_attr;
union ibv_gid;

/* ---- Completion channel (MVP: empty implementation) ---- */
struct ibv_comp_channel {
    struct ibv_context *context;
    int fd;
};

/* ---- Data structures ---- */

/* ---- Forward types (handle struct dependencies) ---- */

union ibv_gid {
    uint8_t  raw[16];
    struct {
        uint64_t subnet_prefix;
        uint64_t interface_id;
    } global;
};

struct ibv_global_route {
    union ibv_gid dgid;
    uint32_t flow_label;
    uint8_t  sgid_index;
    uint8_t  hop_limit;
    uint8_t  traffic_class;
};

struct ibv_qp_cap {
    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_send_sge;
    uint32_t max_recv_sge;
    uint32_t max_inline_data;
};

struct ibv_ah_attr {
    struct ibv_global_route grh;
    uint16_t dlid;
    uint8_t  sl;
    uint8_t  src_path_bits;
    uint8_t  static_rate;
    uint8_t  is_global;
    uint8_t  port_num;
};

/* ---- Full handle structs ---- */

struct ibv_pd {
    struct ibv_context *context;
    uint32_t handle;
    uint32_t pd;              /* maps to libmlx mlx_pd */
};

struct ibv_mr {
    struct ibv_context *context;
    struct ibv_pd *pd;
    void *addr;
    size_t length;
    uint32_t handle;
    uint32_t lkey;
    uint32_t rkey;
};

struct ibv_cq {
    struct ibv_context *context;
    struct ibv_comp_channel *channel;
    void *cq_context;
    uint32_t handle;
    int cqe;
    int comp_vector;
    uint32_t cons_index;
    /* libmlx mapping */
    struct mlx_cq *mlx_cq;
};

struct ibv_qp {
    struct ibv_context *context;
    void *qp_context;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_srq *srq;
    uint32_t handle;
    uint32_t qp_num;
    enum ibv_qp_state state;
    enum ibv_qp_type qp_type;
    struct ibv_qp_cap cap;
    /* libmlx mapping */
    struct mlx_qp *mlx_qp;
    void *sq_buf;
    void *rq_buf;
};

struct ibv_ah {
    struct ibv_context *context;
    struct ibv_pd *pd;
    uint32_t handle;
    struct ibv_ah_attr attr;
};

/* ---- Data structures ---- */

struct ibv_device {
    struct ibv_context *context;
    char name[IBV_SYSFS_NAME_MAX];
    char dev_name[IBV_SYSFS_NAME_MAX];
};

struct ibv_context {
    struct ibv_device *device;
    int cmd_fd;                 /* maps to the MlxUserClient connection */
};

struct ibv_device_attr {
    uint64_t fw_ver;
    uint64_t node_guid;
    uint64_t sys_image_guid;
    uint64_t max_mr_size;
    uint64_t page_size_cap;
    uint32_t vendor_id;
    uint32_t vendor_part_id;
    uint32_t hw_ver;
    int      max_qp;
    int      max_qp_wr;
    unsigned max_sge;
    int      max_sge_rd;
    int      max_cq;
    int      max_cqe;
    int      max_mr;
    int      max_pd;
    int      max_qp_rd_atom;
    int      max_ee_rd_atom;
    int      max_res_rd_atom;
    int      max_qp_init_rd_atom;
    int      max_ee_init_rd_atom;
    int      max_ee;
    int      max_rdd;
    int      max_mw;
    int      max_raw_ipv6_qp;
    int      max_raw_ethy_qp;
    int      max_mcast_grp;
    int      max_mcast_qp_attach;
    int      max_total_mcast_qp_attach;
    int      max_ah;
    int      max_fmr;
    int      max_map_per_fmr;
    int      max_srq;
    int      max_srq_wr;
    int      max_srq_sge;
    uint16_t max_pkeys;
    uint8_t  local_ca_ack_delay;
    uint8_t  atomic_cap;
    uint8_t  phys_port_cnt;
    uint8_t  num_comp_vectors;
};

struct ibv_port_attr {
    enum ibv_port_state state;
    enum ibv_mtu max_mtu;
    enum ibv_mtu active_mtu;
    int      gid_tbl_len;
    uint32_t port_cap_flags;
    uint32_t max_msg_sz;
    uint32_t bad_pkey_cntr;
    uint32_t qkey_viol_cntr;
    uint16_t pkey_tbl_len;
    uint16_t lid;
    uint16_t sm_lid;
    uint8_t  lmc;
    uint8_t  link_layer;
    uint8_t  max_vl_num;
    uint8_t  sm_sl;
    uint8_t  subnet_timeout;
    uint8_t  init_type_reply;
    uint8_t  active_width;
    uint8_t  active_speed;
    uint8_t  phys_state;
    uint8_t  port_cap_flags_roce;
};



struct ibv_grh {
    uint32_t version_tclass_flow;
    uint16_t paylen;
    uint8_t  next_hdr;
    uint8_t  hop_limit;
    union ibv_gid sgid;
    union ibv_gid dgid;
};

struct ibv_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

struct ibv_send_wr {
    uint64_t wr_id;
    struct ibv_send_wr *next;
    struct ibv_sge *sg_list;
    int num_sge;
    enum ibv_wr_opcode opcode;
    int send_flags;
    uint32_t imm_data;
    union {
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
        } rdma;
        struct {
            uint64_t remote_addr;
            uint64_t compare_add;
            uint64_t swap;
            uint32_t rkey;
        } atomic;
        struct {
            struct ibv_ah *ah;
            uint32_t qp_num;
            uint32_t qkey;
        } ud;
    } wr;
};

struct ibv_recv_wr {
    uint64_t wr_id;
    struct ibv_recv_wr *next;
    struct ibv_sge *sg_list;
    int num_sge;
};

struct ibv_wc {
    uint64_t wr_id;
    enum ibv_wc_status status;
    uint32_t byte_len;
    uint32_t imm_data;
    uint32_t qp_num;
    uint32_t src_qp;
    int wc_flags;
    uint16_t pkey_index;
    uint16_t slid;
    uint8_t  sl;
    uint8_t  dlid_path_bits;
};

struct ibv_qp_attr {
    enum ibv_qp_state qp_state;
    enum ibv_qp_state cur_qp_state;
    enum ibv_mtu path_mtu;
    enum ibv_mig_state path_mig_state;
    uint32_t qkey;
    uint32_t rq_psn;
    uint32_t sq_psn;
    uint32_t dest_qp_num;
    int qp_access_flags;
    struct ibv_qp_cap cap;
    struct ibv_ah_attr ah_attr;
    struct ibv_ah_attr alt_ah_attr;
    uint16_t pkey_index;
    uint16_t alt_pkey_index;
    uint8_t  en_sqd_async_notify;
    uint8_t  sq_draining;
    uint8_t  port_num;
    uint8_t  alt_port_num;
    uint8_t  timeout;
    uint8_t  retry_cnt;
    uint8_t  rnr_retry;
    uint8_t  alt_timeout;
    uint32_t max_rd_atomic;
    uint32_t max_dest_rd_atomic;
    uint32_t min_rnr_timer;
};

struct ibv_qp_init_attr {
    void *qp_context;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_srq *srq;
    struct ibv_qp_cap cap;
    enum ibv_qp_type qp_type;
    int sq_sig_all;
};

/* ---- Device operations ---- */
struct ibv_device **ibv_get_device_list(int *num_devices);
void ibv_free_device_list(struct ibv_device **list);
const char *ibv_get_device_name(struct ibv_device *device);
struct ibv_context *ibv_open_device(struct ibv_device *device);
int ibv_close_device(struct ibv_context *context);
int ibv_query_device(struct ibv_context *context,
                     struct ibv_device_attr *device_attr);
int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                   struct ibv_port_attr *port_attr);

/* ---- PD / MR ---- */
struct ibv_pd *ibv_alloc_pd(struct ibv_context *context);
int ibv_dealloc_pd(struct ibv_pd *pd);
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
                          int access);
int ibv_dereg_mr(struct ibv_mr *mr);

/* ---- CQ ---- */
struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe,
                             void *cq_context,
                             struct ibv_comp_channel *channel,
                             int comp_vector);
int ibv_destroy_cq(struct ibv_cq *cq);

/* ---- QP ---- */
struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                             struct ibv_qp_init_attr *qp_init_attr);
int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                  int attr_mask);
int ibv_destroy_qp(struct ibv_qp *qp);
int ibv_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                 int attr_mask, struct ibv_qp_init_attr *init_attr);

/* ---- AH ---- */
struct ibv_ah *ibv_create_ah(struct ibv_pd *pd, struct ibv_ah_attr *attr);
int ibv_destroy_ah(struct ibv_ah *ah);

/* ---- Data path ---- */
int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                  struct ibv_send_wr **bad_wr);
int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
                  struct ibv_recv_wr **bad_wr);
int ibv_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc);
int ibv_req_notify_cq(struct ibv_cq *cq, int solicited_only);

/* ---- Supplemental API (see rdma-core verbs.h) ---- */
struct ibv_comp_channel *ibv_create_comp_channel(struct ibv_context *context);
int ibv_destroy_comp_channel(struct ibv_comp_channel *channel);
int ibv_query_gid(struct ibv_context *context, uint8_t port_num,
                  int index, union ibv_gid *gid);
int ibv_query_pkey(struct ibv_context *context, uint8_t port_num,
                   int index, uint16_t *pkey);
int ibv_get_async_event(struct ibv_context *context,
                        struct ibv_async_event *event);
int ibv_ack_async_event(struct ibv_async_event *event);
int ibv_get_cq_event(struct ibv_comp_channel *channel,
                     struct ibv_cq **cq, void **cq_context);
int ibv_ack_cq_events(struct ibv_cq *cq, unsigned int nevents);

/* ---- Flags ---- */
enum { IBV_SEND_GENERATE_COMPLETION = (1 << 0) };
enum { IBV_WC_RECV = (1 << 1) };

#endif /* VERBS_H */
