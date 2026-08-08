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

#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/OSByteOrder.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(MlxUserClient, IOUserClient)

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
    fOwnedMr = OSArray::withCapacity(8);
    fOwnedAh = OSArray::withCapacity(8);
    return true;
}

void MlxUserClient::stop(IOService *provider)
{
    if (fRoce) {
        fRoce->release();
        fRoce = NULL;
    }
    super::stop(provider);
}

IOReturn MlxUserClient::clientClose(void)
{
    if (fOwnedQp) { fOwnedQp->release(); fOwnedQp = NULL; }
    if (fOwnedMr) { fOwnedMr->release(); fOwnedMr = NULL; }
    if (fOwnedAh) { fOwnedAh->release(); fOwnedAh = NULL; }
    return super::clientClose();
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
        /* UAR page: userspace writes the doorbell directly (BF register) */
        if (fCore->getUAR() && fCore->getUAR()->getUarMemDesc()) {
            *memory = fCore->getUAR()->getUarMemDesc();
            (*memory)->retain();
            *options = kIOMapReadOnly | kIOMapWriteOnly | kIOMapInhibitCache;
            return kIOReturnSuccess;
        }
        break;
    case kMlxUCMemIndexCqe:
        /* CQ buffer: userspace polls the CQEs directly (zero-copy poll_cq) */
        if (fRoce->getCQ() && fActiveCq) {
            IOMemoryDescriptor *desc =
                fRoce->getCQ()->getCqMemDesc(fActiveCq);
            if (desc) {
                *memory = desc;
                (*memory)->retain();
                *options = kIOMapReadOnly | kIOMapWriteOnly;
                return kIOReturnSuccess;
            }
        }
        break;
    case kMlxUCMemIndexDbRecord:
        /* DB record page: userspace post_send updates the SQ/RQ head pointers
         * See the DB page mapping in Linux uverbs */
        if (fCore->getUAR() && fCore->getUAR()->getDbMemDesc()) {
            *memory = fCore->getUAR()->getDbMemDesc();
            (*memory)->retain();
            *options = kIOMapReadOnly | kIOMapWriteOnly;
            return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    default:
        break;
    }
    return kIOReturnUnsupported;
}

/* ---- method dispatch ---- */

#define MLX_METHOD(name, proc, inSize, outSize) \
    { (IOExternalMethodAction)proc, inSize, outSize, 0, 0 }

const IOExternalMethodDispatch MlxUserClient::sMethods[] = {
    MLX_METHOD(kMlxUCMethodQueryDevice, sQueryDevice, 0, 0),
    MLX_METHOD(kMlxUCMethodCreateQP,    sCreateQP,    sizeof(struct mlx_create_qp_req),
               sizeof(struct mlx_create_qp_resp)),
    MLX_METHOD(kMlxUCMethodModifyQP,    sModifyQP,    sizeof(struct mlx_modify_qp_req), 0),
    MLX_METHOD(kMlxUCMethodDestroyQP,   sDestroyQP,   4, 0),
    MLX_METHOD(kMlxUCMethodCreateCQ,    sCreateCQ,    4, 4),
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

IOExternalMethod *MlxUserClient::getExternalMethodForIndex(UInt32 selector)
{
    if (selector < (sizeof(sMethods) / sizeof(sMethods[0])))
        return (IOExternalMethod *)&sMethods[selector];
    return NULL;
}

IOReturn MlxUserClient::externalMethod(uint32_t selector,
                                       IOExternalMethodArguments *args,
                                       IOExternalMethodDispatch *dispatch,
                                       OSObject *target, void *reference)
{
    return super::externalMethod(selector, args, dispatch, target, reference);
}

/* ---- method implementations ---- */

IOReturn MlxUserClient::sQueryDevice(OSObject *t, void *ref,
                                     IOExternalMethodArguments *args)
{
    MlxUserClient *self = (MlxUserClient *)t;
    return self->queryDevice((struct mlx_query_device_resp *)args->structureOutput);
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
    return self->createCQ(*(uint32_t *)args->structureInput,
                          (uint32_t *)args->structureOutput);
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
    return self->fRoce->getGID() ?
           self->fRoce->getGID()->allocGIDIndex() : kIOReturnNoResources;
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
}

IOReturn MlxUserClient::sFwCmd(OSObject *t, void *ref,
                               IOExternalMethodArguments *args)
{
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
        uint32_t pages = OSReadBigInt32(out, 0x40) & 0xFFFFFF;
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
    uint32_t fwRev = IORead32(self->fCore->getBar0(),
                              offsetof(struct MlxInitSeg, fw_rev));
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
    uint64_t phys = self->fCore->getDMA()->lookupPhys(virt);
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

IOReturn MlxUserClient::createQP(const struct mlx_create_qp_req *req,
                                 struct mlx_create_qp_resp *resp)
{
    if (!req || !resp)
        return kIOReturnBadArgument;
    return fRoce->createQP(req, resp);
}

IOReturn MlxUserClient::modifyQP(const struct mlx_modify_qp_req *req)
{
    if (!req)
        return kIOReturnBadArgument;
    return fRoce->modifyQP(req);
}

IOReturn MlxUserClient::destroyQP(uint32_t qpn)
{
    return fRoce->destroyQP(qpn);
}

IOReturn MlxUserClient::createCQ(uint32_t cqeSize, uint32_t *cqHandle)
{
    if (!cqHandle)
        return kIOReturnBadArgument;
    IOReturn kr = fRoce->createCQ(cqeSize, cqHandle);
    if (kr == kIOReturnSuccess)
        fActiveCq = *cqHandle;   /* record for clientMemoryForType mapping */
    return kr;
}

IOReturn MlxUserClient::destroyCQ(uint32_t cqHandle)
{
    return fRoce->destroyCQ(cqHandle);
}

IOReturn MlxUserClient::regMR(const struct mlx_reg_mr_req *req,
                              struct mlx_reg_mr_resp *resp)
{
    if (!req || !resp)
        return kIOReturnBadArgument;
    return fRoce->regMR(req, resp);
}

IOReturn MlxUserClient::deregMR(uint32_t mrHandle)
{
    return fRoce->deregMR(mrHandle);
}

IOReturn MlxUserClient::createAH(const struct mlx_create_ah_req *req,
                                 struct mlx_create_ah_resp *resp)
{
    if (!req || !resp)
        return kIOReturnBadArgument;
    return fRoce->createAH(req, resp);
}

IOReturn MlxUserClient::destroyAH(uint32_t ahHandle)
{
    return fRoce->destroyAH(ahHandle);
}
