#ifndef MLX_FW_PAGES_HPP
#define MLX_FW_PAGES_HPP

#include <libkern/OSTypes.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <kern/thread_call.h>

#include "MlxEQ.hpp"

#define MLX_FW_PAGE_SIZE          4096U
#define MLX_FW_REQUEST_QUEUE_SIZE 64U
#define MLX_FW_MAX_TRACKED_PAGES  65536U

enum MlxFwPagePhase {
    kMlxFwPageBoot = 1,
    kMlxFwPageInit = 2,
    kMlxFwPageRuntime = 3,
};

enum MlxFwPageOwnership {
    kMlxFwPageHostPrepared = 0,
    kMlxFwPageGivePending,
    kMlxFwPageFirmwareOwned,
    kMlxFwPageReturned,
    kMlxFwPageAmbiguous,
};

enum MlxFwPageFault {
    kMlxFwPageFaultNone = 0,
    kMlxFwPageFaultRequestOverflow,
    kMlxFwPageFaultMalformedEvent,
    kMlxFwPageFaultGiveTimeout,
    kMlxFwPageFaultTakeTimeout,
    kMlxFwPageFaultProtocol,
    kMlxFwPageFaultReclaimTimeout,
};

struct MlxFwPage {
    MlxFwPage *next;
    IOBufferMemoryDescriptor *descriptor;
    IODMACommand *mapping;
    uint64_t dmaAddress;
    uint16_t functionId;
    bool embedded;
    MlxFwPagePhase phase;
    MlxFwPageOwnership ownership;
};

struct MlxFwPageRequest {
    uint16_t functionId;
    bool embedded;
    bool releaseAll;
    int32_t numPages;
};

class MlxPCIDriver;

class MlxFwPages : public OSObject, public MlxEventNotifier {
    OSDeclareDefaultStructors(MlxFwPages)

public:
    bool init(MlxPCIDriver *owner, bool embedded);
    IOReturn satisfyStartupPages(MlxFwPagePhase phase);
    IOReturn startRuntime(MlxEQ *eq);
    void stopRuntimeAndDrain();
    IOReturn reclaimAll(uint32_t timeoutMs);
    void releaseAfterDmaBoundary();
    bool hasOutstandingPages() const;
    bool hasAmbiguousPages() const;
    virtual void handleEvent(uint32_t type, void *eqe) APPLE_KEXT_OVERRIDE;
    virtual OSObject *notifierObject() APPLE_KEXT_OVERRIDE { return this; }
    virtual void free() APPLE_KEXT_OVERRIDE;

private:
    static void workerTrampoline(thread_call_param_t param0,
                                 thread_call_param_t param1);
    void workerMain();
    void scheduleWorker();
    bool enqueueRequest(const MlxFwPageRequest &request);
    bool dequeueRequest(MlxFwPageRequest *request);
    IOReturn processRequest(const MlxFwPageRequest &request);

    IOReturn queryPages(MlxFwPagePhase phase, uint16_t *functionId,
                        bool *embedded, uint32_t *pageCount);
    IOReturn givePages(uint16_t functionId, bool embedded,
                       uint32_t pageCount, MlxFwPagePhase phase,
                       bool notifyFailure);
    IOReturn giveBatch(MlxFwPage **pages, uint32_t count,
                       uint16_t functionId, bool embedded);
    IOReturn takePages(uint16_t functionId, bool embedded,
                       uint32_t requested, uint32_t *returned);
    IOReturn notifyAllocationFailure(uint16_t functionId, bool embedded);
    IOReturn allocatePage(uint16_t functionId, bool embedded,
                          MlxFwPagePhase phase, MlxFwPage **pageOut);
    void releasePage(MlxFwPage *page);
    void insertPage(MlxFwPage *page);
    void removePage(MlxFwPage *page);
    MlxFwPage *findPage(uint64_t dmaAddress, uint16_t functionId,
                        bool embedded);
    void releaseAllForFunction(uint16_t functionId, bool embedded);
    void markFunctionAmbiguous(uint16_t functionId, bool embedded);
    void setFatal(MlxFwPageFault fault);

    MlxPCIDriver *fOwner;
    MlxEQ *fEQ;
    MlxFwPage *fPages;
    IOLock *fOperationLock;
    IOSimpleLock *fRequestLock;
    thread_call_t fWorker;
    MlxFwPageRequest fRequests[MLX_FW_REQUEST_QUEUE_SIZE];
    uint32_t fRequestHead;
    uint32_t fRequestTail;
    uint32_t fRequestCount;
    bool fRuntimeStarted;
    bool fAcceptRequests;
    bool fWorkerScheduled;
    bool fWorkerActive;
    bool fWorkerReschedule;
    bool fStopping;
    bool fEmbedded;
    bool fFatal;
    MlxFwPageFault fFault;
    uint32_t fTrackedPages;
};

#endif
