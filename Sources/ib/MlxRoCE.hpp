/*
 * MlxRoCE.hpp — verbs protocol layer entry point (generic Mellanox family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/infiniband/hw/mlx5/main.c (ops table)
 * Decoupled from hardware models: all hardware access goes through the MlxHCA / MlxPCIDriver interfaces
 */
#ifndef MLX_ROCE_HPP
#define MLX_ROCE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include "MlxHCA.hpp"
#include "MlxWQE.hpp"
#include "MlxEQ.hpp"
#include "MlxUCIO.h"

class MlxPCIDriver;
class MlxQP;
class MlxCQ;
class MlxMR;
class MlxAH;
class MlxGID;
class MlxCC;
class MlxEthernet;

/*
 * verbs device — one RDMA device instance
 * Corresponds to Linux's struct mlx5_ib_dev
 */
class MlxRoCE : public IOService, public MlxEventNotifier {
    OSDeclareDefaultStructors(MlxRoCE)

public:
    virtual bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                   UInt32 type, OSDictionary *properties,
                                   IOUserClient **handler) APPLE_KEXT_OVERRIDE;

    /* ---- verbs operations (called by MlxUserClient) ---- */
    kern_return_t createQP(const struct mlx_create_qp_req *req,
                           struct mlx_create_qp_resp *resp);
    kern_return_t modifyQP(const struct mlx_modify_qp_req *req);
    kern_return_t destroyQP(uint32_t qpn);
    kern_return_t createCQ(uint32_t entries,
                           struct mlx_create_cq_resp *resp);
    kern_return_t destroyCQ(uint32_t cqHandle);
    kern_return_t regMR(const struct mlx_reg_mr_req *req,
                        struct mlx_reg_mr_resp *resp);
    kern_return_t deregMR(uint32_t mrHandle);
    kern_return_t createAH(const struct mlx_create_ah_req *req,
                           struct mlx_create_ah_resp *resp);
    kern_return_t destroyAH(uint32_t ahHandle);
    kern_return_t queryDevice(struct mlx_query_device_resp *resp);
    kern_return_t queryPort(struct mlx_query_port_resp *resp);

    /* async event query (called by MlxUserClient) */
    kern_return_t getAsyncEvent(struct mlx_async_event *event);
    void queueAsyncEvent(uint32_t eventType, uint32_t elementType,
                         uint32_t elementHandle);

    /* access to the core layer */
    MlxPCIDriver *getCore() { return fCore; }
    MlxHCA *getHCA() { return fHCA; }
    MlxGID *getGID() { return fGID; }
    MlxCC *getCC() { return fCC; }
    MlxCQ *getCQ() { return fCQ; }
    MlxQP *getQP() { return fQP; }

private:
    /* staged initialization (see pf_profile, main.c:4287) */
    bool stageInit();
    bool stageCaps();
    bool stageGID();
    bool stageDevRes();

    /* event handling (see mlx5_ib_event, main.c:2743) */
    virtual void handleEvent(uint32_t type, void *eqe) APPLE_KEXT_OVERRIDE;

    MlxPCIDriver *fCore;
    MlxHCA       *fHCA;
    MlxQP        *fQP;
    MlxCQ        *fCQ;
    MlxMR        *fMR;
    MlxAH        *fAH;
    MlxGID       *fGID;
    MlxCC        *fCC;
    MlxEthernet  *fEth;

    OSArray      *fQpTable;
    OSArray      *fCqTable;
    OSArray      *fMrTable;
    IOLock       *fResourceLock;
    IOCommandGate *fCommandGate;
    IOWorkLoop   *fWorkLoop;

    /* async event ring buffer */
    struct mlx_async_event fEventRing[16];
    uint32_t     fEventHead;
    uint32_t     fEventTail;
    IOLock      *fEventLock;
};

#endif /* MLX_ROCE_HPP */
