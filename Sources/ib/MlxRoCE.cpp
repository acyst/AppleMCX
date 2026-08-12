/*
 * MlxRoCE.cpp — verbs protocol layer implementation (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/main.c (staged init pf_profile)
 * Trimmed: minimal verbs set (QP/CQ/MR/AH/GID), supporting RC/UD
 */
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxEQ.hpp"
#include "MlxGID.hpp"
#include "MlxQP.hpp"
#include "MlxCQ.hpp"
#include "MlxMR.hpp"
#include "MlxAH.hpp"
#include "MlxCC.hpp"
#include "MlxRegs.hpp"
#include "MlxWQE.hpp"
#include "MlxUserClient.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#define super IOService
OSDefineMetaClassAndStructors(MlxRoCE, IOService)

IOReturn MlxRoCE::newUserClient(task_t owningTask, void *securityID,
                                UInt32 type, OSDictionary *properties,
                                IOUserClient **handler)
{
    if (!handler || type != 0)
        return kIOReturnBadArgument;
    *handler = NULL;
    MlxUserClient *client = OSTypeAlloc(MlxUserClient);
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type, properties) ||
        !client->attach(this) || !client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnNotPrivileged;
    }
    *handler = client;
    return kIOReturnSuccess;
}

bool MlxRoCE::init(OSDictionary *properties)
{
    if (!super::init(properties))
        return false;

    fCore = NULL;
    fHCA = NULL;
    fQP = NULL;
    fCQ = NULL;
    fMR = NULL;
    fAH = NULL;
    fGID = NULL;
    fCC = NULL;
    fEth = NULL;
    fQpTable = NULL;
    fCqTable = NULL;
    fMrTable = NULL;
    fResourceLock = NULL;
    fCommandGate = NULL;
    fWorkLoop = NULL;
    return true;
}

bool MlxRoCE::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    /* Get the core driver from the core layer nub (see mlx5r_probe, main.c:4484) */
    fCore = OSDynamicCast(MlxPCIDriver, provider);
    if (!fCore) {
        IOLog("MlxRoCE: provider is not the core layer\n");
        return false;
    }
    if (!fCore->rocePublicationAllowed()) {
        IOLog("MlxRoCE: userspace ABI is not enabled; refusing publication\n");
        fCore = NULL;
        return false;
    }
    fCore->retain();
    fHCA = fCore->getHCA();

    /* Staged initialization (see pf_profile, main.c:4287) */
    if (!stageInit() || !stageCaps() || !stageGID() || !stageDevRes()) {
        IOLog("MlxRoCE: initialization failed\n");
        cleanupResources();
        return false;
    }

    /* Register the service for userspace IOServiceOpen
     * Multi-device: expose the deviceName property (mlx5_0/mlx5_1), userspace enumerates by name */
    setProperty("deviceName", fCore->getDevName());
    registerService();

    IOLog("MlxRoCE: verbs device ready (ports=%u)\n", fHCA->caps().numPorts);
    return true;
}

void MlxRoCE::stop(IOService *provider)
{
    cleanupResources();
    super::stop(provider);
}

void MlxRoCE::cleanupResources()
{
    /* Unregister EQ event subscriptions */
    MlxEQ *eq = fCore ? fCore->getEQ() : NULL;
    if (eq) {
        eq->unregisterNotifier(MLX_EVENT_TYPE_WQ_CATAS_ERROR, this);
        eq->unregisterNotifier(MLX_EVENT_TYPE_PATH_MIG, this);
        eq->unregisterNotifier(MLX_EVENT_TYPE_COMM_EST, this);
        eq->unregisterNotifier(MLX_EVENT_TYPE_COMPLETION, this);
        eq->unregisterNotifier(MLX_EVENT_TYPE_DEVICE_FATAL, this);
        eq->unregisterNotifier(MLX_EVENT_TYPE_PORT_STATE_CHANGE, this);
        eq->synchronizeCallbacks();
    }
    if (fQP) { fQP->release(); fQP = NULL; }
    if (fCQ) { fCQ->release(); fCQ = NULL; }
    if (fMR) { fMR->release(); fMR = NULL; }
    if (fAH) { fAH->release(); fAH = NULL; }
    if (fCC) { fCC->release(); fCC = NULL; }
    if (fGID) { fGID->release(); fGID = NULL; }
    if (fResourceLock) { IOLockFree(fResourceLock); fResourceLock = NULL; }
    if (fEventLock) { IOLockFree(fEventLock); fEventLock = NULL; }
    if (fQpTable) { fQpTable->release(); fQpTable = NULL; }
    if (fCqTable) { fCqTable->release(); fCqTable = NULL; }
    if (fMrTable) { fMrTable->release(); fMrTable = NULL; }
    if (fWorkLoop) { fWorkLoop->release(); fWorkLoop = NULL; }
    if (fCore) { fCore->release(); fCore = NULL; }
}

void MlxRoCE::free()
{
    cleanupResources();
    super::free();
}

bool MlxRoCE::stageInit()
{
    /* See MLX5_IB_STAGE_INIT (main.c:3695) */
    fResourceLock = IOLockAlloc();
    fWorkLoop = IOWorkLoop::workLoop();
    fQpTable = OSArray::withCapacity(32);
    fCqTable = OSArray::withCapacity(32);
    fMrTable = OSArray::withCapacity(32);
    fEventLock = IOLockAlloc();
    fEventHead = 0;
    fEventTail = 0;
    if (!fResourceLock || !fWorkLoop || !fQpTable || !fCqTable ||
        !fMrTable || !fEventLock)
        return false;
    return true;
}

bool MlxRoCE::stageCaps()
{
    /* See MLX5_IB_STAGE_CAPS (main.c:3903)
     * MVP: verify the RoCE capability, reject if not supported */
    if (!fHCA->caps().roce) {
        IOLog("MlxRoCE: device does not support RoCE\n");
        return false;
    }
    return true;
}

bool MlxRoCE::stageGID()
{
    /* See MLX5_IB_STAGE_ROCE (main.c:3998)
     * Initialize the GID table + write the default GID */
    fGID = OSTypeAlloc(MlxGID);
    if (!fGID || !fGID->init(this, fHCA->caps().roceMaxGid ? fHCA->caps().roceMaxGid : 128))
        return false;

    /* IB port: GID is driven by the subnet manager, skip the RoCE default GID (reserved for Option C) */
    if (fHCA->caps().isIB()) {
        IOLog("MlxRoCE: IB port, GID pending SM assignment (reserved for Option C)\n");
        return true;
    }

    /* Write the default IPv6 link-local GID (see rdma.c:131 mlx5_rdma_add_roce_addr) */
    uint8_t gid[16], mac[6];
    fGID->getLocalAddr(gid, mac);
    if (fGID->setGID(0, gid, mac, MLX_ROCE_VERSION_1, 0, false, 0) !=
        kIOReturnSuccess)
        return false;
    fGID->startAddressMonitor();
    return true;
}

bool MlxRoCE::stageDevRes()
{
    /* See MLX5_IB_STAGE_DEVICE_RESOURCES (main.c:2829)
     * Create device-level default resources */
    fQP = OSTypeAlloc(MlxQP);
    if (!fQP || !fQP->init(this))
        return false;
    fCQ = OSTypeAlloc(MlxCQ);
    if (!fCQ || !fCQ->init(this))
        return false;
    fMR = OSTypeAlloc(MlxMR);
    if (!fMR || !fMR->init(this))
        return false;
    fAH = OSTypeAlloc(MlxAH);
    if (!fAH || !fAH->init(this))
        return false;
    fCC = OSTypeAlloc(MlxCC);
    if (!fCC || !fCC->init(this))
        return false;

    /* Subscribe to EQ events (see mlx5_ib_stage_dev_notifier_init, main.c:4195) */
    MlxEQ *eq = fCore->getEQ();
    if (!eq)
        return false;
    eq->registerNotifier(MLX_EVENT_TYPE_WQ_CATAS_ERROR, this);
    eq->registerNotifier(MLX_EVENT_TYPE_PATH_MIG, this);
    eq->registerNotifier(MLX_EVENT_TYPE_COMM_EST, this);
    eq->registerNotifier(MLX_EVENT_TYPE_COMPLETION, this);
    eq->registerNotifier(MLX_EVENT_TYPE_DEVICE_FATAL, this);
    eq->registerNotifier(MLX_EVENT_TYPE_PORT_STATE_CHANGE, this);
    return true;
}

/* ---- MlxEventNotifier callback (see mlx5_ib_event, main.c:2743) ---- */

void MlxRoCE::handleEvent(uint32_t type, void *eqe)
{
    MlxEqe *eqePtr = (MlxEqe *)eqe;
    switch (type) {
    case MLX_EVENT_TYPE_WQ_CATAS_ERROR:
        IOLog("MlxRoCE: WQ catastrophic error event\n");
        queueAsyncEvent(MLX_EVENT_QP_FATAL, MLX_ASYNC_ELEMENT_QP, 0);
        break;
    case MLX_EVENT_TYPE_PATH_MIG:
        IOLog("MlxRoCE: path migration event\n");
        queueAsyncEvent(MLX_EVENT_PATH_MIG, MLX_ASYNC_ELEMENT_QP, 0);
        break;
    case MLX_EVENT_TYPE_COMM_EST:
        IOLog("MlxRoCE: communication established event\n");
        queueAsyncEvent(MLX_EVENT_COMM_EST, MLX_ASYNC_ELEMENT_QP, 0);
        break;
    case MLX_EVENT_TYPE_DEVICE_FATAL:
        IOLog("MlxRoCE: device fatal error event\n");
        queueAsyncEvent(MLX_EVENT_DEVICE_FATAL, MLX_ASYNC_ELEMENT_DEVICE, 0);
        break;
    case MLX_EVENT_TYPE_PORT_STATE_CHANGE: {
        /* Raw mlx5 subtypes: 1=down, 4=active, 5=initialized. */
        uint8_t sub = eqePtr ? eqePtr->sub_type : 0;
        IOLog("MlxRoCE: port state change (sub=%u)\n", sub);
        if (sub == 4 || sub == 1)
            queueAsyncEvent(sub == 4 ? MLX_EVENT_PORT_ACTIVE : MLX_EVENT_PORT_ERR,
                            MLX_ASYNC_ELEMENT_PORT, 1);
        else if (sub == 8)
            queueAsyncEvent(MLX_EVENT_GID_CHANGE, MLX_ASYNC_ELEMENT_PORT, 1);
        else if (sub == 9)
            queueAsyncEvent(MLX_EVENT_DEVICE_FATAL, MLX_ASYNC_ELEMENT_PORT, 1);
        break;
    }
    case MLX_EVENT_TYPE_COMPLETION: {
        /* CQ completion event → dispatch to the corresponding CQ (see mlx5_eq_comp_int, eq.c:106)
         * eqe->data.comp.cqn → CQ completion callback */
        uint32_t cqn = 0;
        if (eqePtr)
            cqn = OSSwapBigToHostInt32(eqePtr->data.comp.cqn) & 0xFFFFFF;
        if (fCQ)
            fCQ->handleCompletion(cqn);
        break;
    }
    default:
        break;
    }
}

void MlxRoCE::queueAsyncEvent(uint32_t eventType, uint32_t elementType,
                              uint32_t elementHandle)
{
    /* The ring buffer records async events (see ibv_get_async_event semantics) */
    IOLockLock(fEventLock);
    uint32_t next = (fEventHead + 1) % 16;
    if (next != fEventTail) {   /* not full */
        fEventRing[fEventHead].eventType = eventType;
        fEventRing[fEventHead].elementType = elementType;
        fEventRing[fEventHead].elementHandle = elementHandle;
        fEventHead = next;
    }
    IOLockUnlock(fEventLock);
}

kern_return_t MlxRoCE::getAsyncEvent(struct mlx_async_event *event)
{
    if (!event)
        return kIOReturnBadArgument;
    IOLockLock(fEventLock);
    if (fEventTail == fEventHead) {   /* empty */
        IOLockUnlock(fEventLock);
        return kIOReturnNotFound;     /* no events */
    }
    memcpy(event, &fEventRing[fEventTail], sizeof(struct mlx_async_event));
    fEventTail = (fEventTail + 1) % 16;
    IOLockUnlock(fEventLock);
    return kIOReturnSuccess;
}

/* ---- verbs operations (called by MlxUserClient) ---- */

kern_return_t MlxRoCE::createQP(const struct mlx_create_qp_req *req,
                                struct mlx_create_qp_resp *resp)
{
    return fQP->createQP(req, resp);
}

kern_return_t MlxRoCE::modifyQP(const struct mlx_modify_qp_req *req)
{
    return fQP->modifyQP(req);
}

kern_return_t MlxRoCE::destroyQP(uint32_t qpn)
{
    return fQP->destroyQP(qpn);
}

kern_return_t MlxRoCE::createCQ(uint32_t entries,
                                struct mlx_create_cq_resp *resp)
{
    return fCQ->createCQ(entries, resp);
}

kern_return_t MlxRoCE::destroyCQ(uint32_t cqHandle)
{
    return fCQ->destroyCQ(cqHandle);
}

kern_return_t MlxRoCE::regMR(const struct mlx_reg_mr_req *req,
                             struct mlx_reg_mr_resp *resp)
{
    return fMR->regMR(req, resp);
}

kern_return_t MlxRoCE::deregMR(uint32_t mrHandle)
{
    return fMR->deregMR(mrHandle);
}

kern_return_t MlxRoCE::createAH(const struct mlx_create_ah_req *req,
                                struct mlx_create_ah_resp *resp)
{
    return fAH->createAH(req, resp);
}

kern_return_t MlxRoCE::destroyAH(uint32_t ahHandle)
{
    return fAH->destroyAH(ahHandle);
}

kern_return_t MlxRoCE::queryDevice(struct mlx_query_device_resp *resp)
{
    const MlxHcaCaps &c = fHCA->caps();
    resp->fwVersion = c.fwRev;
    resp->deviceId = fHCA->vendor().deviceId;
    resp->numPorts = c.numPorts;
    resp->maxQp = c.maxQp;
    resp->maxCq = c.maxCq;
    resp->maxMr = c.maxMr;
    resp->roceVersions = c.roceVersions;
    resp->maxGid = c.roceMaxGid;
    resp->maxMsgSize = 1u << 20;
    return kIOReturnSuccess;
}

kern_return_t MlxRoCE::queryPort(struct mlx_query_port_resp *resp)
{
    resp->portNum = 1;
    resp->portState = 1;        /* up */
    resp->activeSpeed = 10000;
    resp->maxMtu = 9000;

    /* Distinguish by link layer (Option C: supports IB ports) */
    if (fHCA->caps().isIB()) {
        resp->linkLayer = MLX_LINK_LAYER_INFINIBAND;   /* 1=IB */
        resp->gidType = 0;
        resp->lid = 0;           /* to be assigned by SM (MVP) */
        resp->smLid = 0;
        resp->pkeyTblLen = fHCA->caps().ibMaxPkeys ? fHCA->caps().ibMaxPkeys : 16;
        resp->gidTblLen = fHCA->caps().roceMaxGid ? fHCA->caps().roceMaxGid : 128;
    } else {
        resp->linkLayer = MLX_LINK_LAYER_ETHERNET;     /* 2=Ethernet */
        resp->gidType = 2;          /* RoCEv2 */
        resp->pkeyTblLen = 16;
        resp->gidTblLen = fHCA->caps().roceMaxGid ? fHCA->caps().roceMaxGid : 128;
    }
    return kIOReturnSuccess;
}
