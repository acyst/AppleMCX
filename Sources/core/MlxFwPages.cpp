#include "MlxFwPages.hpp"

#include "MlxCmd.hpp"
#include "MlxKernelCompat.hpp"
#include "MlxP1Encoding.hpp"
#include "MlxPCIDriver.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>
#include <string.h>

#define super OSObject
OSDefineMetaClassAndStructors(MlxFwPages, OSObject)

bool MlxFwPages::init(MlxPCIDriver *owner, bool embedded)
{
    if (!super::init() || !owner)
        return false;
    fOwner = owner;
    fEQ = NULL;
    fPages = NULL;
    fOperationLock = IOLockAlloc();
    fRequestLock = IOSimpleLockAlloc();
    fWorker = thread_call_allocate_with_options(
        &MlxFwPages::workerTrampoline, this, THREAD_CALL_PRIORITY_KERNEL,
        THREAD_CALL_OPTIONS_ONCE);
    fRequestHead = fRequestTail = fRequestCount = 0;
    fRuntimeStarted = false;
    fAcceptRequests = false;
    fWorkerScheduled = false;
    fWorkerActive = false;
    fWorkerReschedule = false;
    fStopping = false;
    fEmbedded = embedded;
    fFatal = false;
    fFault = kMlxFwPageFaultNone;
    fTrackedPages = 0;
    return fOperationLock && fRequestLock && fWorker;
}

void MlxFwPages::free()
{
    stopRuntimeAndDrain();
    if (fWorker) {
        thread_call_cancel(fWorker);
        while (thread_call_isactive(fWorker))
            IOSleep(1);
        thread_call_free(fWorker);
        fWorker = NULL;
    }
    MlxFwPage *page = fPages;
    while (page) {
        MlxFwPage *next = page->next;
        if (page->ownership == kMlxFwPageHostPrepared ||
            page->ownership == kMlxFwPageReturned)
            releasePage(page);
        page = next;
    }
    if (fOperationLock) {
        IOLockFree(fOperationLock);
        fOperationLock = NULL;
    }
    if (fRequestLock) {
        IOSimpleLockFree(fRequestLock);
        fRequestLock = NULL;
    }
    super::free();
}

IOReturn MlxFwPages::queryPages(MlxFwPagePhase phase, uint16_t *functionId,
                                bool *embedded, uint32_t *pageCount)
{
    if (!functionId || !embedded || !pageCount ||
        (phase != kMlxFwPageBoot && phase != kMlxFwPageInit))
        return kIOReturnBadArgument;
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    if (!mlxP1EncodeQueryPages(in, sizeof(in), static_cast<uint16_t>(phase),
                               fEmbedded))
        return kIOReturnBadArgument;
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_QUERY_PAGES };
    IOReturn kr = fOwner->getCmd()->exec(&cmd, 5000);
    if (kr != kIOReturnSuccess)
        return kr;
    int32_t pages = static_cast<int32_t>(static_cast<uint32_t>(
        mlxGetBits(out, 0x60, 32)));
    if (pages < 0)
        return kIOReturnIOError;
    *functionId = static_cast<uint16_t>(mlxGetBits(out, 0x50, 16));
    *embedded = mlxGetBits(out, 0x40, 1) != 0;
    *pageCount = static_cast<uint32_t>(pages);
    return kIOReturnSuccess;
}

IOReturn MlxFwPages::satisfyStartupPages(MlxFwPagePhase phase)
{
    uint16_t functionId = 0;
    bool embedded = false;
    uint32_t pageCount = 0;
    IOLockLock(fOperationLock);
    IOReturn kr = queryPages(phase, &functionId, &embedded, &pageCount);
    if (kr == kIOReturnSuccess && pageCount)
        kr = givePages(functionId, embedded, pageCount, phase, false);
    IOLockUnlock(fOperationLock);
    return kr;
}

IOReturn MlxFwPages::allocatePage(uint16_t functionId, bool embedded,
                                  MlxFwPagePhase phase, MlxFwPage **pageOut)
{
    if (!pageOut)
        return kIOReturnBadArgument;
    *pageOut = NULL;
    MlxFwPage *page = static_cast<MlxFwPage *>(IOMallocZero(sizeof(*page)));
    if (!page)
        return kIOReturnNoMemory;
    page->descriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, MLX_FW_PAGE_SIZE, 0xFFFFFFF000ULL);
    if (!page->descriptor) {
        IOFree(page, sizeof(*page));
        return kIOReturnNoMemory;
    }
    void *bytes = page->descriptor->getBytesNoCopy();
    if (!bytes) {
        page->descriptor->release();
        IOFree(page, sizeof(*page));
        return kIOReturnNoMemory;
    }
    memset(bytes, 0, MLX_FW_PAGE_SIZE);
    page->mapping = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, MLX_FW_PAGE_SIZE,
        IODMACommand::kMapped, MLX_FW_PAGE_SIZE, MLX_FW_PAGE_SIZE);
    if (!page->mapping ||
        page->mapping->setMemoryDescriptor(page->descriptor) != kIOReturnSuccess) {
        if (page->mapping) page->mapping->release();
        page->descriptor->release();
        IOFree(page, sizeof(*page));
        return kIOReturnNoMemory;
    }
    UInt64 offset = 0;
    IODMACommand::Segment64 segment = {};
    UInt32 count = 1;
    IOReturn kr = page->mapping->gen64IOVMSegments(&offset, &segment, &count);
    if (kr != kIOReturnSuccess || count != 1 ||
        segment.fLength < MLX_FW_PAGE_SIZE || !segment.fIOVMAddr ||
        (segment.fIOVMAddr & (MLX_FW_PAGE_SIZE - 1))) {
        mlxUnmapDMA(page->mapping);
        page->mapping = NULL;
        page->descriptor->release();
        IOFree(page, sizeof(*page));
        return kIOReturnNoSpace;
    }
    page->dmaAddress = segment.fIOVMAddr;
    page->functionId = functionId;
    page->embedded = embedded;
    page->phase = phase;
    page->ownership = kMlxFwPageHostPrepared;
    insertPage(page);
    fTrackedPages++;
    *pageOut = page;
    return kIOReturnSuccess;
}

void MlxFwPages::insertPage(MlxFwPage *page)
{
    page->next = fPages;
    fPages = page;
}

void MlxFwPages::removePage(MlxFwPage *page)
{
    MlxFwPage **link = &fPages;
    while (*link && *link != page)
        link = &(*link)->next;
    if (*link)
        *link = page->next;
}

void MlxFwPages::releasePage(MlxFwPage *page)
{
    if (!page)
        return;
    removePage(page);
    if (fTrackedPages)
        fTrackedPages--;
    mlxUnmapDMA(page->mapping);
    if (page->descriptor)
        page->descriptor->release();
    IOFree(page, sizeof(*page));
}

IOReturn MlxFwPages::giveBatch(MlxFwPage **pages, uint32_t count,
                               uint16_t functionId, bool embedded)
{
    uint32_t inputSize = mlxP1ManagePagesSize(count);
    if (!pages || !count || !inputSize)
        return kIOReturnBadArgument;
    uint8_t *in = static_cast<uint8_t *>(IOMallocZero(inputSize));
    uint64_t *addresses = static_cast<uint64_t *>(
        IOMallocZero(sizeof(uint64_t) * count));
    if (!in || !addresses) {
        if (in) IOFree(in, inputSize);
        if (addresses) IOFree(addresses, sizeof(uint64_t) * count);
        return kIOReturnNoMemory;
    }
    for (uint32_t i = 0; i < count; i++) {
        addresses[i] = pages[i]->dmaAddress;
        pages[i]->ownership = kMlxFwPageGivePending;
    }
    mlxP1EncodeManagePages(in, inputSize, MLX_P1_PAGES_GIVE, functionId,
                           embedded, addresses, count);
    uint8_t out[16] = {};
    MlxCmdInOut cmd = { in, inputSize, out, sizeof(out),
                        MLX_CMD_OP_MANAGE_PAGES };
    IOReturn kr = fOwner->getCmd()->exec(&cmd, 5000);
    for (uint32_t i = 0; i < count; i++) {
        if (kr == kIOReturnSuccess)
            pages[i]->ownership = kMlxFwPageFirmwareOwned;
        else if (kr == kIOReturnTimeout)
            pages[i]->ownership = kMlxFwPageAmbiguous;
        else
            pages[i]->ownership = kMlxFwPageHostPrepared;
    }
    if (kr == kIOReturnTimeout)
        setFatal(kMlxFwPageFaultGiveTimeout);
    IOFree(addresses, sizeof(uint64_t) * count);
    IOFree(in, inputSize);
    return kr;
}

IOReturn MlxFwPages::givePages(uint16_t functionId, bool embedded,
                               uint32_t pageCount, MlxFwPagePhase phase,
                               bool notifyFailure)
{
    if (!pageCount)
        return kIOReturnSuccess;
    if (pageCount > MLX_FW_MAX_TRACKED_PAGES ||
        fTrackedPages > MLX_FW_MAX_TRACKED_PAGES - pageCount) {
        if (notifyFailure)
            notifyAllocationFailure(functionId, embedded);
        return kIOReturnNoResources;
    }
    MlxFwPage **pages = static_cast<MlxFwPage **>(
        IOMallocZero(sizeof(MlxFwPage *) * pageCount));
    MlxFwPage **batch = static_cast<MlxFwPage **>(
        IOMallocZero(sizeof(MlxFwPage *) * MLX_P1_MAX_MANAGE_PAGES));
    if (!pages || !batch) {
        if (pages) IOFree(pages, sizeof(MlxFwPage *) * pageCount);
        if (batch) IOFree(batch, sizeof(MlxFwPage *) * MLX_P1_MAX_MANAGE_PAGES);
        if (notifyFailure) notifyAllocationFailure(functionId, embedded);
        return kIOReturnNoMemory;
    }
    uint32_t allocated = 0;
    IOReturn kr = kIOReturnSuccess;
    for (; allocated < pageCount; allocated++) {
        kr = allocatePage(functionId, embedded, phase, &pages[allocated]);
        if (kr != kIOReturnSuccess)
            break;
    }
    if (kr != kIOReturnSuccess) {
        for (uint32_t i = 0; i < allocated; i++)
            releasePage(pages[i]);
        if (notifyFailure) notifyAllocationFailure(functionId, embedded);
        goto out;
    }
    for (uint32_t start = 0; start < pageCount; start += MLX_P1_MAX_MANAGE_PAGES) {
        uint32_t count = pageCount - start;
        if (count > MLX_P1_MAX_MANAGE_PAGES)
            count = MLX_P1_MAX_MANAGE_PAGES;
        for (uint32_t i = 0; i < count; i++)
            batch[i] = pages[start + i];
        kr = giveBatch(batch, count, functionId, embedded);
        if (kr != kIOReturnSuccess) {
            for (uint32_t i = start; i < pageCount; i++) {
                if (pages[i]->ownership == kMlxFwPageHostPrepared)
                    releasePage(pages[i]);
            }
            if (start)
                setFatal(kMlxFwPageFaultProtocol);
            break;
        }
    }
out:
    IOFree(batch, sizeof(MlxFwPage *) * MLX_P1_MAX_MANAGE_PAGES);
    IOFree(pages, sizeof(MlxFwPage *) * pageCount);
    return kr;
}

IOReturn MlxFwPages::notifyAllocationFailure(uint16_t functionId, bool embedded)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxP1EncodeManagePages(in, sizeof(in), MLX_P1_PAGES_ALLOCATION_FAIL,
                           functionId, embedded, NULL, 0);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_MANAGE_PAGES };
    return fOwner->getCmd()->exec(&cmd, 5000);
}

MlxFwPage *MlxFwPages::findPage(uint64_t dmaAddress, uint16_t functionId,
                                bool embedded)
{
    for (MlxFwPage *page = fPages; page; page = page->next) {
        if (page->dmaAddress == dmaAddress && page->functionId == functionId &&
            page->embedded == embedded)
            return page;
    }
    return NULL;
}

void MlxFwPages::markFunctionAmbiguous(uint16_t functionId, bool embedded)
{
    for (MlxFwPage *page = fPages; page; page = page->next) {
        if (page->functionId == functionId && page->embedded == embedded &&
            page->ownership == kMlxFwPageFirmwareOwned)
            page->ownership = kMlxFwPageAmbiguous;
    }
}

IOReturn MlxFwPages::takePages(uint16_t functionId, bool embedded,
                               uint32_t requested, uint32_t *returned)
{
    if (!requested || requested > MLX_P1_MAX_MANAGE_PAGES)
        return kIOReturnBadArgument;
    if (returned) *returned = 0;
    uint8_t in[16] = {};
    uint32_t outputSize = mlxP1ManagePagesSize(requested);
    uint8_t *out = static_cast<uint8_t *>(IOMallocZero(outputSize));
    MlxFwPage **validated = static_cast<MlxFwPage **>(
        IOMallocZero(sizeof(MlxFwPage *) * requested));
    if (!out || !validated) {
        if (out) IOFree(out, outputSize);
        if (validated) IOFree(validated, sizeof(MlxFwPage *) * requested);
        return kIOReturnNoMemory;
    }
    mlxP1EncodeManagePages(in, sizeof(in), MLX_P1_PAGES_TAKE, functionId,
                           embedded, NULL, requested);
    MlxCmdInOut cmd = { in, sizeof(in), out, outputSize,
                        MLX_CMD_OP_MANAGE_PAGES };
    IOReturn kr = fOwner->getCmd()->exec(&cmd, 5000);
    if (kr == kIOReturnTimeout) {
        markFunctionAmbiguous(functionId, embedded);
        setFatal(kMlxFwPageFaultTakeTimeout);
        goto out;
    }
    if (kr != kIOReturnSuccess)
        goto out;
    uint32_t count = static_cast<uint32_t>(mlxGetBits(out, 0x40, 32));
    if (count > requested) {
        kr = kIOReturnIOError;
        goto protocol;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint64_t address = mlxGetBits(out, 0x80 + i * 64, 64);
        MlxFwPage *page = findPage(address, functionId, embedded);
        if (!address || (address & (MLX_FW_PAGE_SIZE - 1)) || !page ||
            page->ownership != kMlxFwPageFirmwareOwned) {
            kr = kIOReturnIOError;
            goto protocol;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (validated[j] == page) {
                kr = kIOReturnIOError;
                goto protocol;
            }
        }
        validated[i] = page;
    }
    for (uint32_t i = 0; i < count; i++) {
        validated[i]->ownership = kMlxFwPageReturned;
        releasePage(validated[i]);
    }
    if (returned) *returned = count;
    goto out;
protocol:
    markFunctionAmbiguous(functionId, embedded);
    setFatal(kMlxFwPageFaultProtocol);
out:
    IOFree(validated, sizeof(MlxFwPage *) * requested);
    IOFree(out, outputSize);
    return kr;
}

IOReturn MlxFwPages::reclaimAll(uint32_t timeoutMs)
{
    IOLockLock(fOperationLock);
    uint32_t waited = 0;
    IOReturn result = kIOReturnSuccess;
    while (true) {
        MlxFwPage *first = NULL;
        uint32_t count = 0;
        for (MlxFwPage *page = fPages; page; page = page->next) {
            if (page->ownership != kMlxFwPageFirmwareOwned)
                continue;
            if (!first)
                first = page;
            if (page->functionId == first->functionId &&
                page->embedded == first->embedded)
                count++;
        }
        if (!first)
            break;
        if (count > MLX_P1_MAX_MANAGE_PAGES)
            count = MLX_P1_MAX_MANAGE_PAGES;
        uint32_t returned = 0;
        result = takePages(first->functionId, first->embedded, count, &returned);
        if (result != kIOReturnSuccess)
            break;
        if (!returned) {
            if (waited >= timeoutMs) {
                markFunctionAmbiguous(first->functionId, first->embedded);
                setFatal(kMlxFwPageFaultReclaimTimeout);
                result = kIOReturnTimeout;
                break;
            }
            IOSleep(50);
            waited += 50;
        } else {
            waited = 0;
        }
    }
    if (hasAmbiguousPages() && result == kIOReturnSuccess)
        result = kIOReturnNotReady;
    IOLockUnlock(fOperationLock);
    return result;
}

void MlxFwPages::releaseAllForFunction(uint16_t functionId, bool embedded)
{
    MlxFwPage *page = fPages;
    while (page) {
        MlxFwPage *next = page->next;
        if (page->functionId == functionId && page->embedded == embedded &&
            page->ownership == kMlxFwPageFirmwareOwned) {
            page->ownership = kMlxFwPageReturned;
            releasePage(page);
        }
        page = next;
    }
}

bool MlxFwPages::hasOutstandingPages() const
{
    for (MlxFwPage *page = fPages; page; page = page->next) {
        if (page->ownership == kMlxFwPageFirmwareOwned ||
            page->ownership == kMlxFwPageAmbiguous ||
            page->ownership == kMlxFwPageGivePending)
            return true;
    }
    return false;
}

bool MlxFwPages::hasAmbiguousPages() const
{
    for (MlxFwPage *page = fPages; page; page = page->next) {
        if (page->ownership == kMlxFwPageAmbiguous ||
            page->ownership == kMlxFwPageGivePending)
            return true;
    }
    return false;
}

void MlxFwPages::releaseAfterDmaBoundary()
{
    IOLockLock(fOperationLock);
    MlxFwPage *page = fPages;
    while (page) {
        MlxFwPage *next = page->next;
        releasePage(page);
        page = next;
    }
    IOLockUnlock(fOperationLock);
}

void MlxFwPages::setFatal(MlxFwPageFault fault)
{
    fFatal = true;
    fFault = fault;
}

bool MlxFwPages::enqueueRequest(const MlxFwPageRequest &request)
{
    bool queued = false;
    IOSimpleLockLock(fRequestLock);
    if (fAcceptRequests && !fStopping &&
        fRequestCount < MLX_FW_REQUEST_QUEUE_SIZE) {
        fRequests[fRequestTail] = request;
        fRequestTail = (fRequestTail + 1) % MLX_FW_REQUEST_QUEUE_SIZE;
        fRequestCount++;
        queued = true;
    }
    IOSimpleLockUnlock(fRequestLock);
    return queued;
}

bool MlxFwPages::dequeueRequest(MlxFwPageRequest *request)
{
    bool dequeued = false;
    IOSimpleLockLock(fRequestLock);
    if (fRequestCount) {
        *request = fRequests[fRequestHead];
        fRequestHead = (fRequestHead + 1) % MLX_FW_REQUEST_QUEUE_SIZE;
        fRequestCount--;
        dequeued = true;
    }
    IOSimpleLockUnlock(fRequestLock);
    return dequeued;
}

void MlxFwPages::scheduleWorker()
{
    bool schedule = false;
    IOSimpleLockLock(fRequestLock);
    if (fWorkerActive) {
        fWorkerReschedule = true;
    } else if (!fWorkerScheduled) {
        fWorkerScheduled = true;
        schedule = true;
    }
    IOSimpleLockUnlock(fRequestLock);
    if (schedule)
        thread_call_enter(fWorker);
}

void MlxFwPages::workerTrampoline(thread_call_param_t param0,
                                  thread_call_param_t param1)
{
    (void)param1;
    MlxFwPages *self = static_cast<MlxFwPages *>(param0);
    if (self)
        self->workerMain();
}

void MlxFwPages::workerMain()
{
    IOSimpleLockLock(fRequestLock);
    fWorkerScheduled = false;
    fWorkerActive = true;
    IOSimpleLockUnlock(fRequestLock);
    MlxFwPageRequest request = {};
    while (dequeueRequest(&request)) {
        if (processRequest(request) != kIOReturnSuccess) {
            IOSimpleLockLock(fRequestLock);
            fRequestHead = fRequestTail;
            fRequestCount = 0;
            IOSimpleLockUnlock(fRequestLock);
            break;
        }
    }
    IOSimpleLockLock(fRequestLock);
    bool again = (fWorkerReschedule || fRequestCount) && !fStopping;
    fWorkerActive = false;
    fWorkerReschedule = false;
    if (again)
        fWorkerScheduled = true;
    IOSimpleLockUnlock(fRequestLock);
    if (again)
        thread_call_enter(fWorker);
    if (fFatal)
        fOwner->enterDmaQuarantine(static_cast<uint32_t>(fFault));
}

IOReturn MlxFwPages::processRequest(const MlxFwPageRequest &request)
{
    IOLockLock(fOperationLock);
    IOReturn kr = kIOReturnSuccess;
    if (request.releaseAll) {
        releaseAllForFunction(request.functionId, request.embedded);
    } else if (request.numPages > 0) {
        kr = givePages(request.functionId, request.embedded,
                       static_cast<uint32_t>(request.numPages),
                       kMlxFwPageRuntime, true);
    } else if (request.numPages < 0 && request.numPages != INT32_MIN) {
        uint32_t remaining = static_cast<uint32_t>(-request.numPages);
        while (remaining) {
            uint32_t batch = remaining > MLX_P1_MAX_MANAGE_PAGES ?
                MLX_P1_MAX_MANAGE_PAGES : remaining;
            uint32_t returned = 0;
            kr = takePages(request.functionId, request.embedded, batch, &returned);
            if (kr != kIOReturnSuccess)
                break;
            if (!returned) {
                markFunctionAmbiguous(request.functionId, request.embedded);
                setFatal(kMlxFwPageFaultProtocol);
                kr = kIOReturnIOError;
                break;
            }
            remaining -= returned;
        }
    } else {
        kr = kIOReturnBadArgument;
        setFatal(kMlxFwPageFaultMalformedEvent);
    }
    IOLockUnlock(fOperationLock);
    return kr;
}

IOReturn MlxFwPages::startRuntime(MlxEQ *eq)
{
    if (!eq || fRuntimeStarted)
        return kIOReturnBadArgument;
    fEQ = eq;
    fStopping = false;
    fAcceptRequests = true;
    fEQ->registerNotifier(MLX_EVENT_TYPE_PAGE_REQUEST, this);
    fRuntimeStarted = true;
    return kIOReturnSuccess;
}

void MlxFwPages::stopRuntimeAndDrain()
{
    if (!fRuntimeStarted)
        return;
    IOSimpleLockLock(fRequestLock);
    fAcceptRequests = false;
    fStopping = true;
    IOSimpleLockUnlock(fRequestLock);
    if (fEQ)
        fEQ->unregisterNotifier(MLX_EVENT_TYPE_PAGE_REQUEST, this);
    if (fEQ)
        fEQ->synchronizeCallbacks();
    scheduleWorker();
    while (true) {
        IOSimpleLockLock(fRequestLock);
        bool drained = !fWorkerActive && !fWorkerScheduled &&
                       !fWorkerReschedule && !fRequestCount;
        IOSimpleLockUnlock(fRequestLock);
        if (drained)
            break;
        IOSleep(1);
    }
    thread_call_cancel(fWorker);
    while (thread_call_isactive(fWorker))
        IOSleep(1);
    fRuntimeStarted = false;
    fEQ = NULL;
}

void MlxFwPages::handleEvent(uint32_t type, void *data)
{
    if (type != MLX_EVENT_TYPE_PAGE_REQUEST || !data)
        return;
    MlxEqe *eqe = static_cast<MlxEqe *>(data);
    uint16_t flags = OSSwapBigToHostInt16(eqe->data.page_request.ec_function);
    MlxFwPageRequest request = {};
    request.functionId = OSSwapBigToHostInt16(
        eqe->data.page_request.function_id);
    request.embedded = (flags & 0x8000) != 0;
    request.releaseAll = (flags & 0x4000) != 0;
    request.numPages = static_cast<int32_t>(OSSwapBigToHostInt32(
        eqe->data.page_request.num_pages));
    if ((!request.releaseAll && !request.numPages) ||
        request.numPages == INT32_MIN) {
        setFatal(kMlxFwPageFaultMalformedEvent);
        scheduleWorker();
        return;
    }
    if (!enqueueRequest(request))
        setFatal(kMlxFwPageFaultRequestOverflow);
    scheduleWorker();
}
