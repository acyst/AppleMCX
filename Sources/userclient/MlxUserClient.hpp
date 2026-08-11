/*
 * MlxUserClient.hpp — userspace interface (IOUserClient)
 *
 * Corresponds to Linux /dev/infiniband/uverbsX + ioctl + mmap
 * macOS: IOConnectCallMethod (control path) + IOConnectMapMemory (UAR/DB mapping)
 */
#ifndef MLX_USER_CLIENT_HPP
#define MLX_USER_CLIENT_HPP

#include <IOKit/IOUserClient.h>
#include "MlxUCIO.h"
#include "MlxHCA.hpp"

class MlxRoCE;
class MlxPCIDriver;

/*
 * User client — one instance per user process
 */
class MlxUserClient : public IOUserClient {
    OSDeclareDefaultStructors(MlxUserClient)

public:
    /* IOService lifecycle */
    virtual bool initWithTask(task_t owningTask, void *securityToken,
                              UInt32 type,
                              OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* IOUserClient */
    virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientMemoryForType(UInt32 type,
                                         IOOptionBits *options,
                                         IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;

    /* method table */
    static const IOExternalMethodDispatch sMethods[];
    static IOReturn sQueryDevice(OSObject *t, void *ref,
                                 IOExternalMethodArguments *args);
    static IOReturn sQueryPort(OSObject *t, void *ref,
                               IOExternalMethodArguments *args);
    static IOReturn sCreateQP(OSObject *t, void *ref,
                              IOExternalMethodArguments *args);
    static IOReturn sModifyQP(OSObject *t, void *ref,
                              IOExternalMethodArguments *args);
    static IOReturn sDestroyQP(OSObject *t, void *ref,
                               IOExternalMethodArguments *args);
    static IOReturn sCreateCQ(OSObject *t, void *ref,
                              IOExternalMethodArguments *args);
    static IOReturn sDestroyCQ(OSObject *t, void *ref,
                               IOExternalMethodArguments *args);
    static IOReturn sRegMR(OSObject *t, void *ref,
                           IOExternalMethodArguments *args);
    static IOReturn sDeregMR(OSObject *t, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sCreateAH(OSObject *t, void *ref,
                              IOExternalMethodArguments *args);
    static IOReturn sDestroyAH(OSObject *t, void *ref,
                               IOExternalMethodArguments *args);
    static IOReturn sGetGidIndex(OSObject *t, void *ref,
                                 IOExternalMethodArguments *args);
    static IOReturn sCCQuery(OSObject *t, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sCCModify(OSObject *t, void *ref,
                              IOExternalMethodArguments *args);
    /* firmware management */
    static IOReturn sAccessReg(OSObject *t, void *ref,
                               IOExternalMethodArguments *args);
    static IOReturn sFwCmd(OSObject *t, void *ref,
                           IOExternalMethodArguments *args);
    static IOReturn sQueryPages(OSObject *t, void *ref,
                                IOExternalMethodArguments *args);
    static IOReturn sPortStats(OSObject *t, void *ref,
                               IOExternalMethodArguments *args);
    static IOReturn sFwReset(OSObject *t, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sQueryFwVer(OSObject *t, void *ref,
                                IOExternalMethodArguments *args);
    static IOReturn sQueryHealth(OSObject *t, void *ref,
                                 IOExternalMethodArguments *args);
    /* DMA data path */
    static IOReturn sVirtToPhys(OSObject *t, void *ref,
                                IOExternalMethodArguments *args);
    /* completion events */
    static IOReturn sQueryCqCompletions(OSObject *t, void *ref,
                                        IOExternalMethodArguments *args);
    /* async events */
    static IOReturn sGetAsyncEvent(OSObject *t, void *ref,
                                   IOExternalMethodArguments *args);
    /* CQ consumer index update */
    static IOReturn sUpdateCqConsumer(OSObject *t, void *ref,
                                      IOExternalMethodArguments *args);

private:
    void cleanup();
    void releaseOwnedResources();
    bool addOwned(OSArray *table, uint32_t handle);
    bool owns(OSArray *table, uint32_t handle);
    bool removeOwned(OSArray *table, uint32_t handle);
    bool takeOwned(OSArray *table, uint32_t *handle);

    IOReturn queryDevice(struct mlx_query_device_resp *resp);
    IOReturn queryPort(struct mlx_query_port_resp *resp);
    IOReturn createQP(const struct mlx_create_qp_req *req,
                      struct mlx_create_qp_resp *resp);
    IOReturn modifyQP(const struct mlx_modify_qp_req *req);
    IOReturn destroyQP(uint32_t qpn);
    IOReturn createCQ(uint32_t entries,
                      struct mlx_create_cq_resp *resp);
    IOReturn destroyCQ(uint32_t cqHandle);
    IOReturn updateCqConsumer(uint32_t cqHandle, uint32_t consumerIndex);
    IOReturn regMR(const struct mlx_reg_mr_req *req,
                   struct mlx_reg_mr_resp *resp);
    IOReturn deregMR(uint32_t mrHandle);
    IOReturn createAH(const struct mlx_create_ah_req *req,
                      struct mlx_create_ah_resp *resp);
    IOReturn destroyAH(uint32_t ahHandle);

    MlxRoCE       *fRoce;
    MlxPCIDriver  *fCore;
    task_t          fOwningTask;
    OSArray       *fOwnedQp;    /* QPs owned by the client */
    OSArray       *fOwnedCq;
    OSArray       *fOwnedMr;
    OSArray       *fOwnedAh;
    IOLock        *fOwnedLock;
    uint32_t       fActiveCq;   /* most recently mapped CQ (used by clientMemoryForType) */
};

#endif /* MLX_USER_CLIENT_HPP */
