/*
 * MlxUserClient.cpp — userspace interface implementation (IOUserClient)
 *
 * Corresponds to Linux /dev/infiniband/uverbsX + ioctl + mmap
 * macOS: IOConnectCallMethod (control path) + IOConnectMapMemory (UAR/DB mapping)
 */
#include "MlxUserClient.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxCmd.hpp"
#include "MlxUAR.hpp"
#include "MlxHCA.hpp"
#include "MlxGID.hpp"
#include "MlxCC.hpp"
#include "MlxCQ.hpp"
#include "MlxHealth.hpp"
#include "MlxDMA.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"

#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/OSByteOrder.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(MlxUserClient, IOUserClient)

bool MlxUserClient::initWithTask(task_t owningTask, void *securityToken,
                                 UInt32 type, OSDictionary *properties)
{
    if (IOUserClient::clientHasPrivilege(
            securityToken, kIOClientPrivilegeAdministrator) !=
        kIOReturnSuccess)
        return false;
    if (!super::initWithTask(owningTask, securityToken, type, properties))
        return false;
    fOwningTask = owningTask;
    fRoce = NULL;
    fCore = NULL;
    fOwnedQp = NULL;
    fOwnedCq = NULL;
    fOwnedMr = NULL;
    fOwnedAh = NULL;
    fOwnedLock = NULL;
    fActiveCq = 0;
    return true;
}

bool MlxUserClient::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    fRoce = OSDynamicCast(MlxRoCE, provider);
    if (!fRoce) {
        IOLog("MlxUserClient: provider is not MlxRoCE\n");
        return false;
    }
    fRoce->retain();
    fCore = fRoce->getCore();
    fOwnedQp = OSArray::withCapacity(8);
    fOwnedCq = OSArray::withCapacity(8);
    fOwnedMr = OSArray::withCapacity(8);
    fOwnedAh = OSArray::withCapacity(8);
    fOwnedLock = IOLockAlloc();
    if (!fOwnedQp || !fOwnedCq || !fOwnedMr || !fOwnedAh || !fOwnedLock) {
        cleanup();
        return false;
    }
    return true;
}

void MlxUserClient::stop(IOService *provider)
{
    cleanup();
    super::stop(provider);
}

void MlxUserClient::free()
{
    cleanup();
    super::free();
}

IOReturn MlxUserClient::clientClose(void)
{
    cleanup();
    return super::clientClose();
}

void MlxUserClient::cleanup()
{
    releaseOwnedResources();
    if (fOwnedQp) { fOwnedQp->release(); fOwnedQp = NULL; }
    if (fOwnedCq) { fOwnedCq->release(); fOwnedCq = NULL; }
    if (fOwnedMr) { fOwnedMr->release(); fOwnedMr = NULL; }
    if (fOwnedAh) { fOwnedAh->release(); fOwnedAh = NULL; }
    if (fOwnedLock) { IOLockFree(fOwnedLock); fOwnedLock = NULL; }
    if (fRoce) { fRoce->release(); fRoce = NULL; }
    fCore = NULL;
}

bool MlxUserClient::addOwned(OSArray *table, uint32_t handle)
{
    if (!table || !fOwnedLock)
        return false;
    OSData *record = OSData::withBytes(&handle, sizeof(handle));
    if (!record)
        return false;
    IOLockLock(fOwnedLock);
    bool added = table->setObject(record);
    IOLockUnlock(fOwnedLock);
    record->release();
    return added;
}

bool MlxUserClient::removeOwned(OSArray *table, uint32_t handle)
{
    if (!table || !fOwnedLock)
        return false;
    bool found = false;
    IOLockLock(fOwnedLock);
    for (uint32_t i = 0; i < table->getCount(); i++) {
        OSData *record = OSDynamicCast(OSData, table->getObject(i));
        if (!record || record->getLength() != sizeof(handle) ||
            *static_cast<const uint32_t *>(record->getBytesNoCopy()) != handle)
            continue;
        table->removeObject(i);
        found = true;
        break;
    }
    IOLockUnlock(fOwnedLock);
    return found;
}

bool MlxUserClient::owns(OSArray *table, uint32_t handle)
{
    if (!table || !fOwnedLock)
        return false;
    bool found = false;
    IOLockLock(fOwnedLock);
    for (uint32_t i = 0; i < table->getCount(); i++) {
        OSData *record = OSDynamicCast(OSData, table->getObject(i));
        if (record && record->getLength() == sizeof(handle) &&
            *static_cast<const uint32_t *>(record->getBytesNoCopy()) == handle) {
            found = true;
            break;
        }
    }
    IOLockUnlock(fOwnedLock);
    return found;
}

bool MlxUserClient::takeOwned(OSArray *table, uint32_t *handle)
{
    if (!table || !handle || !fOwnedLock)
        return false;
    bool found = false;
    IOLockLock(fOwnedLock);
    if (table->getCount()) {
        OSData *record = OSDynamicCast(OSData, table->getObject(0));
        if (record && record->getLength() == sizeof(*handle)) {
            *handle = *static_cast<const uint32_t *>(record->getBytesNoCopy());
            found = true;
        }
        table->removeObject(static_cast<unsigned int>(0));
    }
    IOLockUnlock(fOwnedLock);
    return found;
}

void MlxUserClient::releaseOwnedResources()
{
    if (!fRoce)
        return;
    uint32_t handle;
    while (takeOwned(fOwnedQp, &handle)) fRoce->destroyQP(handle);
    while (takeOwned(fOwnedMr, &handle)) fRoce->deregMR(handle);
    while (takeOwned(fOwnedAh, &handle)) fRoce->destroyAH(handle);
    while (takeOwned(fOwnedCq, &handle)) fRoce->destroyCQ(handle);
    fActiveCq = 0;
}

IOReturn MlxUserClient::clientMemoryForType(UInt32 type,
                                            IOOptionBits *options,
                                            IOMemoryDescriptor **memory)
{
    /* Userspace mmap: the key to the zero-copy data path
     * See Linux uverbs mmap (UAR page + DB record) */
    if (!options || !memory)
        return kIOReturnBadArgument;

    *memory = NULL;
    switch (type) {
    case kMlxUCMemIndexUar:
        /* The current core UAR is shared by the device, not by this client.
         * Do not expose it until per-client UAR allocation exists. */
        return kIOReturnUnsupported;
    case kMlxUCMemIndexCqe:
        /* CQ buffer: userspace polls the CQEs directly (zero-copy poll_cq) */
        if (fRoce->getCQ() && fActiveCq) {
            IOMemoryDescriptor *desc =
                fRoce->getCQ()->getCqMemDesc(fActiveCq);
            if (desc) {
                *memory = desc;
                *options = 0;
                return kIOReturnSuccess;
            }
        }
        break;
    case kMlxUCMemIndexDbRecord:
        /* The page contains other clients' DB slots. A per-client mapping
         * must be implemented before exposing it. */
        return kIOReturnUnsupported;
    default:
        break;
    }
    return kIOReturnUnsupported;
}

/* ---- method dispatch ---- */

#define MLX_METHOD(name, proc, inSize, outSize) \
    { (IOExternalMethodAction)proc, 0, inSize, 0, outSize }

const IOExternalMethodDispatch MlxUserClient::sMethods[] = {
    MLX_METHOD(kMlxUCMethodQueryDevice, sQueryDevice, 0,
               sizeof(struct mlx_query_device_resp)),
    MLX_METHOD(kMlxUCMethodQueryPort,   sQueryPort, 0,
               sizeof(struct mlx_query_port_resp)),
    MLX_METHOD(kMlxUCMethodCreateQP,    sCreateQP,    sizeof(struct mlx_create_qp_req),
               sizeof(struct mlx_create_qp_resp)),
    MLX_METHOD(kMlxUCMethodModifyQP,    sModifyQP,    sizeof(struct mlx_modify_qp_req), 0),
    MLX_METHOD(kMlxUCMethodDestroyQP,   sDestroyQP,   4, 0),
    MLX_METHOD(kMlxUCMethodCreateCQ,    sCreateCQ,
               sizeof(struct mlx_create_cq_req),
               sizeof(struct mlx_create_cq_resp)),
    MLX_METHOD(kMlxUCMethodDestroyCQ,   sDestroyCQ,   4, 0),
    MLX_METHOD(kMlxUCMethodRegMR,       sRegMR,       sizeof(struct mlx_reg_mr_req),
               sizeof(struct mlx_reg_mr_resp)),
    MLX_METHOD(kMlxUCMethodDeregMR,     sDeregMR,     4, 0),
    MLX_METHOD(kMlxUCMethodCreateAH,    sCreateAH,    sizeof(struct mlx_create_ah_req),
               sizeof(struct mlx_create_ah_resp)),
    MLX_METHOD(kMlxUCMethodDestroyAH,   sDestroyAH,   4, 0),
    MLX_METHOD(kMlxUCMethodGetGidIndex, sGetGidIndex, 0, 4),
    MLX_METHOD(kMlxUCMethodCCQuery,     sCCQuery,     0, sizeof(struct mlx_cc_params)),
    MLX_METHOD(kMlxUCMethodCCModify,    sCCModify,    sizeof(struct mlx_cc_params), 0),
    /* firmware management */
    MLX_METHOD(kMlxUCMethodAccessReg,   sAccessReg,   sizeof(struct mlx_access_reg_req),
               sizeof(struct mlx_access_reg_resp)),
    MLX_METHOD(kMlxUCMethodFwCmd,       sFwCmd,       sizeof(struct mlx_fw_cmd_req),
               sizeof(struct mlx_fw_cmd_resp)),
    MLX_METHOD(kMlxUCMethodQueryPages,  sQueryPages,  0, 8),
    MLX_METHOD(kMlxUCMethodPortStats,   sPortStats,   0, sizeof(struct mlx_port_stats_resp)),
    MLX_METHOD(kMlxUCMethodFwReset,     sFwReset,     0, 0),
    MLX_METHOD(kMlxUCMethodQueryFwVer,  sQueryFwVer,  0, sizeof(struct mlx_fw_ver_resp)),
    MLX_METHOD(kMlxUCMethodQueryHealth, sQueryHealth, 0, 4),
    /* DMA data path */
    MLX_METHOD(kMlxUCMethodVirtToPhys,  sVirtToPhys,  8, 8),
    /* completion events */
    MLX_METHOD(kMlxUCMethodQueryCqCompletions, sQueryCqCompletions, 4, 8),
    /* async events */
    MLX_METHOD(kMlxUCMethodGetAsyncEvent, sGetAsyncEvent, 0,
               sizeof(struct mlx_async_event)),
};

IOReturn MlxUserClient::externalMethod(uint32_t selector,
                                       IOExternalMethodArguments *args,
                                       IOExternalMethodDispatch *dispatch,
                                       OSObject *target, void *reference)
{
    struct SelectorMap {
        uint32_t selector;
        uint32_t index;
    };
    static const SelectorMap map[] = {
        { kMlxUCMethodQueryDevice, 0 }, { kMlxUCMethodQueryPort, 1 },
        { kMlxUCMethodCreateQP, 2 }, { kMlxUCMethodModifyQP, 3 },
        { kMlxUCMethodDestroyQP, 4 }, { kMlxUCMethodCreateCQ, 5 },
        { kMlxUCMethodDestroyCQ, 6 }, { kMlxUCMethodRegMR, 7 },
        { kMlxUCMethodDeregMR, 8 }, { kMlxUCMethodCreateAH, 9 },
        { kMlxUCMethodDestroyAH, 10 }, { kMlxUCMethodGetGidIndex, 11 },
        { kMlxUCMethodCCQuery, 12 }, { kMlxUCMethodCCModify, 13 },
        { kMlxUCMethodAccessReg, 14 }, { kMlxUCMethodFwCmd, 15 },
        { kMlxUCMethodQueryPages, 16 }, { kMlxUCMethodPortStats, 17 },
        { kMlxUCMethodFwReset, 18 }, { kMlxUCMethodQueryFwVer, 19 },
        { kMlxUCMethodQueryHealth, 20 }, { kMlxUCMethodVirtToPhys, 21 },
        { kMlxUCMethodQueryCqCompletions, 22 },
        { kMlxUCMethodGetAsyncEvent, 23 },
    };
    for (uint32_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].selector != selector)
            continue;
        return super::externalMethod(selector, args,
                                     const_cast<IOExternalMethodDispatch *>(
                                         &sMethods[map[i].index]),
                                     this, reference);
    }
    return kIOReturnUnsupported;
}

/* ---- method implementations ---- */

IOReturn MlxUserClient::sQueryDevice(OSObject *t, void *ref,
                                     IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->queryDevice((struct mlx_query_device_resp *)args->structureOutput);
}

IOReturn MlxUserClient::sQueryPort(OSObject *t, void *ref,
                                   IOExternalMethodArguments *args)
{
    MlxUserClient *self = static_cast<MlxUserClient *>(t);
    return self->queryPort(
        static_cast<struct mlx_query_port_resp *>(args->structureOutput));
}

IOReturn MlxUserClient::sCreateQP(OSObject *t, void *ref,
                                  IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->createQP((struct mlx_create_qp_req *)args->structureInput,
                          (struct mlx_create_qp_resp *)args->structureOutput);
}

IOReturn MlxUserClient::sModifyQP(OSObject *t, void *ref,
                                  IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->modifyQP((struct mlx_modify_qp_req *)args->structureInput);
}

IOReturn MlxUserClient::sDestroyQP(OSObject *t, void *ref,
                                   IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->destroyQP(*(uint32_t *)args->structureInput);
}

IOReturn MlxUserClient::sCreateCQ(OSObject *t, void *ref,
                                  IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    const struct mlx_create_cq_req *req =
        (const struct mlx_create_cq_req *)args->structureInput;
    return self->createCQ(req->entries,
                          (struct mlx_create_cq_resp *)args->structureOutput);
}

IOReturn MlxUserClient::sDestroyCQ(OSObject *t, void *ref,
                                   IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->destroyCQ(*(uint32_t *)args->structureInput);
}

IOReturn MlxUserClient::sRegMR(OSObject *t, void *ref,
                               IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->regMR((struct mlx_reg_mr_req *)args->structureInput,
                       (struct mlx_reg_mr_resp *)args->structureOutput);
}

IOReturn MlxUserClient::sDeregMR(OSObject *t, void *ref,
                                 IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->deregMR(*(uint32_t *)args->structureInput);
}

IOReturn MlxUserClient::sCreateAH(OSObject *t, void *ref,
                                  IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->createAH((struct mlx_create_ah_req *)args->structureInput,
                          (struct mlx_create_ah_resp *)args->structureOutput);
}

IOReturn MlxUserClient::sDestroyAH(OSObject *t, void *ref,
                                   IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->destroyAH(*(uint32_t *)args->structureInput);
}

IOReturn MlxUserClient::sGetGidIndex(OSObject *t, void *ref,
                                     IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    if (!args->structureOutput || !self->fRoce->getGID())
        return kIOReturnNoResources;
    uint32_t index = self->fRoce->getGID()->allocGIDIndex();
    if (index == 0xFFFFFFFF)
        return kIOReturnNoResources;
    *(uint32_t *)args->structureOutput = index;
    return kIOReturnSuccess;
}

IOReturn MlxUserClient::sCCQuery(OSObject *t, void *ref,
                                 IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->fRoce->getCC() ?
           self->fRoce->getCC()->queryParams((struct mlx_cc_params *)args->structureOutput)
           : kIOReturnNoResources;
}

IOReturn MlxUserClient::sCCModify(OSObject *t, void *ref,
                                  IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->fRoce->getCC() ?
           self->fRoce->getCC()->modifyParams((struct mlx_cc_params *)args->structureInput)
           : kIOReturnNoResources;
}

/* ---- firmware management methods ---- */

IOReturn MlxUserClient::sAccessReg(OSObject *t, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)t;
    (void)ref;
    (void)args;
    return kIOReturnUnsupported;
#if 0
    /* ACCESS_REG: pass through the firmware command directly (see core/port.c mlx5_access_reg)
     * MVP: build the ACCESS_REG command and send it through the command interface */
    MlxUserClient *self = (MlxUserClient *)t;
    struct mlx_access_reg_req *req =
        (struct mlx_access_reg_req *)args->structureInput;
    struct mlx_access_reg_resp *resp =
        (struct mlx_access_reg_resp *)args->structureOutput;
    if (!req || !resp)
        return kIOReturnBadArgument;

    /* Build the ACCESS_REG command (opcode 0x805) */
    uint8_t in[512] = {};
    uint8_t out[512] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_ACCESS_REG);
    OSWriteBigInt16(in, 2, req->opMod);        /* op_mod */
    OSWriteBigInt32(in, 4, req->argument);
    OSWriteBigInt32(in, 8, req->registerId);
    if (req->opMod == 1 && req->dataSize) {
        /* write: data is in register_data */
        memcpy(in + 16, req->data, (req->dataSize < 240) ? req->dataSize : 240);
    }
    uint32_t outSize = 512;
    kern_return_t kr = self->fCore->exec(MLX_CMD_OP_ACCESS_REG, in, 512,
                                         out, outSize, 5000);
    if (kr != kIOReturnSuccess)
        return kr;
    if (req->opMod == 0) {
        memcpy(resp->data, out + 16, 240);
    }
    resp->dataSize = 240;
    return kIOReturnSuccess;
#endif
}

IOReturn MlxUserClient::sFwCmd(OSObject *t, void *ref,
                               IOExternalMethodArguments *args)
{
    (void)t;
    (void)ref;
    (void)args;
    return kIOReturnUnsupported;
#if 0
    /* Firmware command passthrough (used by mlxup): any opcode */
    MlxUserClient *self = (MlxUserClient *)t;
    struct mlx_fw_cmd_req *req = (struct mlx_fw_cmd_req *)args->structureInput;
    struct mlx_fw_cmd_resp *resp = (struct mlx_fw_cmd_resp *)args->structureOutput;
    if (!req || !resp)
        return kIOReturnBadArgument;

    uint8_t in[512] = {};
    memcpy(in, req->in, (req->inSize < 512) ? req->inSize : 512);
    OSWriteBigInt16(in, 0, req->opcode);
    uint8_t out[512] = {};
    uint32_t outSize = req->inSize;
    kern_return_t kr = self->fCore->exec(req->opcode, in, req->inSize,
                                         out, outSize, 10000);
    if (kr != kIOReturnSuccess)
        return kr;
    memcpy(resp->out, out, (outSize < 512) ? outSize : 512);
    resp->outSize = outSize;
    return kIOReturnSuccess;
#endif
}

IOReturn MlxUserClient::sQueryPages(OSObject *t, void *ref,
                                    IOExternalMethodArguments *args)
{
    /* QUERY_PAGES (firmware page management) */
    MlxUserClient *self = (MlxUserClient *)t;
    uint8_t in[16] = {};
    uint8_t out[32] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_QUERY_PAGES);
    OSWriteBigInt16(in, 2, 1);   /* op_mod: BOOT pages */
    kern_return_t kr = self->fCore->exec(MLX_CMD_OP_QUERY_PAGES, in, 16,
                                         out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        return kr;
    if (args->structureOutput) {
        uint32_t pages = static_cast<uint32_t>(mlxGetBits(out, 0x60, 32));
        *(uint64_t *)args->structureOutput = pages;
    }
    return kIOReturnSuccess;
}

IOReturn MlxUserClient::sPortStats(OSObject *t, void *ref,
                                   IOExternalMethodArguments *args)
{
    /* Port statistics (used by mlxlink): query the PPCR/PPCNT registers */
    MlxUserClient *self = (MlxUserClient *)t;
    struct mlx_port_stats_resp *resp =
        (struct mlx_port_stats_resp *)args->structureOutput;
    if (!resp)
        return kIOReturnBadArgument;
    memset(resp, 0, sizeof(*resp));
    resp->portNum = 1;
    resp->linkState = 1;
    resp->linkSpeed = 10000;
    /* MVP: full counters are read from the PPCR register, later in P5 */
    return kIOReturnSuccess;
}

IOReturn MlxUserClient::sFwReset(OSObject *t, void *ref,
                                 IOExternalMethodArguments *args)
{
    /* Firmware reset (used by mlxfwreset) */
    MlxUserClient *self = (MlxUserClient *)t;
    (void)self;
    /* MVP: online reset is not supported */
    return kIOReturnUnsupported;
}

IOReturn MlxUserClient::sQueryFwVer(OSObject *t, void *ref,
                                    IOExternalMethodArguments *args)
{
    /* Firmware version query */
    MlxUserClient *self = (MlxUserClient *)t;
    struct mlx_fw_ver_resp *resp =
        (struct mlx_fw_ver_resp *)args->structureOutput;
    if (!resp)
        return kIOReturnBadArgument;
    memset(resp, 0, sizeof(*resp));
    uint32_t fwRev = mlxMMIORead32BE(
        self->fCore->getBar0(), offsetof(struct MlxInitSeg, fw_rev));
    resp->fwRev = fwRev;
    resp->cmdifRev = self->fCore->getCmd()->cmdifRev();
    if (self->fCore->getHCA()) {
        const MlxVendorInfo &v = self->fCore->getHCA()->vendor();
        resp->deviceId = v.deviceId;
        resp->portType = self->fCore->getHCA()->caps().portType;
        resp->numPorts = self->fCore->getHCA()->caps().numPorts;
    } else {
        resp->deviceId = 0x1017;   /* fallback: default ConnectX-5 */
        resp->portType = 1;
        resp->numPorts = 1;
    }
    return kIOReturnSuccess;
}

IOReturn MlxUserClient::sQueryHealth(OSObject *t, void *ref,
                                     IOExternalMethodArguments *args)
{
    /* health status */
    MlxUserClient *self = (MlxUserClient *)t;
    if (!args->structureOutput)
        return kIOReturnBadArgument;
    *(uint32_t *)args->structureOutput =
        self->fCore->getHealth() ? (uint32_t)self->fCore->getHealth()->isHealthy()
                                 : 1;
    return kIOReturnSuccess;
}

IOReturn MlxUserClient::sVirtToPhys(OSObject *t, void *ref,
                                    IOExternalMethodArguments *args)
{
    /* Virtual address → physical address (used by the post_send data segment)
     * Input: structureInput = uint64_t virtual address
     * Output: structureOutput = uint64_t physical address */
    MlxUserClient *self = (MlxUserClient *)t;
    if (!args->structureInput || !args->structureOutput)
        return kIOReturnBadArgument;
    uint64_t virt = *(uint64_t *)args->structureInput;
    uint64_t phys = self->fCore->getDMA()->lookupPhys(
        self->fOwningTask, virt);
    if (phys == 0)
        return kIOReturnNotFound;
    *(uint64_t *)args->structureOutput = phys;
    return kIOReturnSuccess;
}

IOReturn MlxUserClient::sQueryCqCompletions(OSObject *t, void *ref,
                                            IOExternalMethodArguments *args)
{
    /* Query the CQ completion count (used by ibv_get_cq_event)
     * Input: structureInput = uint32_t cqHandle
     * Output: structureOutput = uint64_t completion count */
    MlxUserClient *self = (MlxUserClient *)t;
    if (!args->structureInput || !args->structureOutput)
        return kIOReturnBadArgument;
    uint32_t cqHandle = *(uint32_t *)args->structureInput;
    if (!self->owns(self->fOwnedCq, cqHandle))
        return kIOReturnNotPermitted;
    uint64_t count = self->fRoce->getCQ()->getCompletions(cqHandle);
    *(uint64_t *)args->structureOutput = count;
    return kIOReturnSuccess;
}

IOReturn MlxUserClient::sGetAsyncEvent(OSObject *t, void *ref,
                                       IOExternalMethodArguments *args)
{
    /* Get an async event (non-blocking)
     * Output: structureOutput = mlx_async_event
     * Returns kIOReturnNotFound when there are no events (userspace maps it to EAGAIN) */
    MlxUserClient *self = (MlxUserClient *)t;
    if (!args->structureOutput)
        return kIOReturnBadArgument;
    return self->fRoce->getAsyncEvent(
        (struct mlx_async_event *)args->structureOutput);
}

/* ---- internal implementations ---- */

IOReturn MlxUserClient::queryDevice(struct mlx_query_device_resp *resp)
{
    if (!resp)
        return kIOReturnBadArgument;
    return fRoce->queryDevice(resp);
}

IOReturn MlxUserClient::queryPort(struct mlx_query_port_resp *resp)
{
    if (!fRoce || !resp)
        return kIOReturnBadArgument;
    return fRoce->queryPort(resp);
}

IOReturn MlxUserClient::createQP(const struct mlx_create_qp_req *req,
                                 struct mlx_create_qp_resp *resp)
{
    if (!req || !resp)
        return kIOReturnBadArgument;
    if (req->pd != 1 || !owns(fOwnedCq, req->sendCq) ||
        !owns(fOwnedCq, req->recvCq))
        return kIOReturnNotPermitted;
    IOReturn kr = fRoce->createQP(req, resp);
    if (kr == kIOReturnSuccess && !addOwned(fOwnedQp, resp->qpn)) {
        fRoce->destroyQP(resp->qpn);
        return kIOReturnNoMemory;
    }
    return kr;
}

IOReturn MlxUserClient::modifyQP(const struct mlx_modify_qp_req *req)
{
    if (!req)
        return kIOReturnBadArgument;
    if (!removeOwned(fOwnedQp, req->qpn))
        return kIOReturnNotPermitted;
    IOReturn kr = fRoce->modifyQP(req);
    addOwned(fOwnedQp, req->qpn);
    return kr;
}

IOReturn MlxUserClient::destroyQP(uint32_t qpn)
{
    if (!removeOwned(fOwnedQp, qpn))
        return kIOReturnNotPermitted;
    IOReturn kr = fRoce->destroyQP(qpn);
    if (kr != kIOReturnSuccess) addOwned(fOwnedQp, qpn);
    return kr;
}

IOReturn MlxUserClient::createCQ(uint32_t entries,
                                 struct mlx_create_cq_resp *resp)
{
    if (!fRoce || !resp || fActiveCq != 0)
        return kIOReturnBadArgument;
    memset(resp, 0, sizeof(*resp));
    IOReturn kr = fRoce->createCQ(entries, resp);
    if (kr == kIOReturnSuccess && !addOwned(fOwnedCq, resp->cqHandle)) {
        fRoce->destroyCQ(resp->cqHandle);
        return kIOReturnNoMemory;
    }
    if (kr == kIOReturnSuccess)
        fActiveCq = resp->cqHandle;
    return kr;
}

IOReturn MlxUserClient::destroyCQ(uint32_t cqHandle)
{
    if (!removeOwned(fOwnedCq, cqHandle))
        return kIOReturnNotPermitted;
    /* The current mmap ABI exposes one CQ buffer per client. More
     * importantly, a live QP may still reference this CQ in firmware. */
    if (fOwnedQp && fOwnedQp->getCount() != 0) {
        addOwned(fOwnedCq, cqHandle);
        return kIOReturnBusy;
    }
    IOReturn kr = fRoce->destroyCQ(cqHandle);
    if (kr != kIOReturnSuccess) addOwned(fOwnedCq, cqHandle);
    if (kr == kIOReturnSuccess && fActiveCq == cqHandle) fActiveCq = 0;
    return kr;
}

IOReturn MlxUserClient::regMR(const struct mlx_reg_mr_req *req,
                              struct mlx_reg_mr_resp *resp)
{
    if (!req || !resp)
        return kIOReturnBadArgument;
    IOReturn kr = fRoce->regMR(req, resp);
    if (kr == kIOReturnSuccess && !addOwned(fOwnedMr, resp->mrHandle)) {
        fRoce->deregMR(resp->mrHandle);
        return kIOReturnNoMemory;
    }
    return kr;
}

IOReturn MlxUserClient::deregMR(uint32_t mrHandle)
{
    if (!removeOwned(fOwnedMr, mrHandle))
        return kIOReturnNotPermitted;
    IOReturn kr = fRoce->deregMR(mrHandle);
    if (kr != kIOReturnSuccess) addOwned(fOwnedMr, mrHandle);
    return kr;
}

IOReturn MlxUserClient::createAH(const struct mlx_create_ah_req *req,
                                 struct mlx_create_ah_resp *resp)
{
    if (!req || !resp)
        return kIOReturnBadArgument;
    IOReturn kr = fRoce->createAH(req, resp);
    if (kr == kIOReturnSuccess && !addOwned(fOwnedAh, resp->ahHandle)) {
        fRoce->destroyAH(resp->ahHandle);
        return kIOReturnNoMemory;
    }
    return kr;
}

IOReturn MlxUserClient::destroyAH(uint32_t ahHandle)
{
    if (!removeOwned(fOwnedAh, ahHandle))
        return kIOReturnNotPermitted;
    IOReturn kr = fRoce->destroyAH(ahHandle);
    if (kr != kIOReturnSuccess) addOwned(fOwnedAh, ahHandle);
    return kr;
}
