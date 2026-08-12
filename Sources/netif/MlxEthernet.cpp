/*
 * MlxEthernet.cpp — Ethernet interface implementation (generic Mellanox family)
 *
 * Port reference: mlx5e (en_main.c / en_tx.c / en_rx.c)
 * macOS: IOEthernetController attaches to the kernel protocol stack
 *
 * Note: this file is the interface registration + TX transmit framework.
 *       Full hardware queue (SQ/RQ) configuration (CREATE_SQ/CREATE_RQ/TIR/TIS) comes later in P2.
 */
#include "MlxEthernet.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.hpp"
#include "MlxKernelCompat.hpp"
#include "MlxCmd.hpp"
#include "MlxDMA.hpp"
#include "MlxDoorbell.hpp"
#include "MlxRegs.hpp"

#include <string.h>
#include <sys/mbuf.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>
#include <libkern/OSByteOrder.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(MlxEthernet, IOEthernetController)

#define super2 IOService
OSDefineMetaClassAndStructors(MlxEthernetDriver, IOService)

/* ========== MlxEthRing ========== */

#define super3 OSObject
OSDefineMetaClassAndStructors(MlxEthRing, OSObject)

bool MlxEthRing::init(uint32_t size, uint32_t wqebbSize)
{
    if (!super3::init())
        return false;
    fSize = size;
    fWqebbSize = wqebbSize;
    fHead = 0;
    fTail = 0;
    fWqBuf = NULL;
    fWqDMA = 0;
    fWqDesc = NULL;
    fWqDmaMap = NULL;
    fDbRecord = NULL;
    fDbDMA = 0;
    memset(fWqeMbuf, 0, sizeof(fWqeMbuf));
    return true;
}

void MlxEthRing::free()
{
    freeBuffers();
    super3::free();
}

kern_return_t MlxEthRing::allocBuffers()
{
    /* Allocate a DMA-coherent WQ buffer + DB record (see mlx5_wq_alloc) */
    uint32_t wqBytes = fSize * fWqebbSize;
    fWqDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, wqBytes, 0xFFFFFFF000ULL);
    if (!fWqDesc)
        return kIOReturnNoMemory;
    memset(fWqDesc->getBytesNoCopy(), 0, wqBytes);
    fWqBuf = fWqDesc->getBytesNoCopy();
    if (mlxMapDMAContiguous(fWqDesc, &fWqDmaMap, &fWqDMA) !=
        kIOReturnSuccess) {
        fWqDesc->release();
        fWqDesc = NULL;
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

void MlxEthRing::freeBuffers()
{
    if (fWqDesc) {
        mlxUnmapDMA(fWqDmaMap);
        fWqDmaMap = NULL;
        fWqDesc->release();
        fWqDesc = NULL;
        fWqBuf = NULL;
        fWqDMA = 0;
    }
    fDbRecord = NULL;
    fDbDMA = 0;
}

void MlxEthRing::updateDb(uint16_t head)
{
    fHead = head;
    if (fDbRecord)
        *fDbRecord = OSSwapHostToBigInt32(head);
    mlxMemoryBarrier();
    /* The doorbell write (BF) is done in the xmit path */
}

void MlxEthRing::setWqeMbuf(uint16_t idx, mbuf_t m)
{
    if (idx < (1 << MLX5E_DEFAULT_LOG_SQ_SIZE))
        fWqeMbuf[idx] = m;
}

mbuf_t MlxEthRing::getWqeMbuf(uint16_t idx)
{
    if (idx < (1 << MLX5E_DEFAULT_LOG_SQ_SIZE))
        return fWqeMbuf[idx];
    return NULL;
}

void MlxEthRing::clearWqeMbuf(uint16_t idx)
{
    if (idx < (1 << MLX5E_DEFAULT_LOG_SQ_SIZE))
        fWqeMbuf[idx] = NULL;
}

/* ========== MlxEthernet ========== */

bool MlxEthernet::init(OSDictionary *properties)
{
    if (!super::init(properties))
        return false;

    fCore = NULL;
    fRoce = NULL;
    fNetif = NULL;
    fNic = NULL;
    fTxRing = NULL;
    fRxRing = NULL;
    fEnabled = false;
    fLinkUp = false;
    fLinkSpeed = 0;
    memset(fMacAddr, 0, 6);
    fTxMinInlineMode = 0;
    fLock = IOLockAlloc();

    /* mlx5e resources */
    fPd = MLX5E_INVALID_RESOURCE;
    fTd = MLX5E_INVALID_RESOURCE;
    fTisn = MLX5E_INVALID_RESOURCE;
    fSqn = MLX5E_INVALID_RESOURCE;
    fRqn = MLX5E_INVALID_RESOURCE;
    fTirn = MLX5E_INVALID_RESOURCE;
    fTxCqn = MLX5E_INVALID_RESOURCE;
    fRxCqn = MLX5E_INVALID_RESOURCE;
    fMkeyIndex = MLX5E_INVALID_RESOURCE;
    fLkey = 0;
    fFlowTableId = MLX5E_INVALID_RESOURCE;
    fFlowGroupId = MLX5E_INVALID_RESOURCE;
    fTxDbOffset = MLX5E_INVALID_RESOURCE;
    fRxDbOffset = MLX5E_INVALID_RESOURCE;
    fTxCqDbOffset = MLX5E_INVALID_RESOURCE;
    fRxCqDbOffset = MLX5E_INVALID_RESOURCE;
    memset(&fTxBf, 0, sizeof(fTxBf));
    fTxBfValid = false;
    fTxCqDesc = NULL;
    fTxCqMap = NULL;
    fTxCqDMA = 0;
    fRxCqDesc = NULL;
    fRxCqMap = NULL;
    fRxCqDMA = 0;
    fTxBufDesc = NULL;
    fTxBufMap = NULL;
    fTxBufDMA = 0;
    fRxBufDesc = NULL;
    fRxBufMap = NULL;
    fRxBufDMA = 0;
    fTxPc = 0;
    fTxCc = 0;
    fRxPc = 0;
    fRxCc = 0;
    fTxCqCc = 0;
    fRxCqCc = 0;
    fTxArmSn = 0;
    fRxArmSn = 0;
    fFlowRootActive = false;
    fFlowEntryValid = false;
    fEnabling = false;
    fDisabling = false;
    fNotifierRegistered = false;
    return true;
}

bool MlxEthernet::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    /* Get the core driver from the provider (core layer nub) */
    fCore = OSDynamicCast(MlxPCIDriver, provider);
    if (!fCore) {
        IOLog("MlxEthernet: provider is not the core layer\n");
        return false;
    }
    fCore->retain();

    /* Allocate the TX/RX ring buffers. Both WQs must remain DMA mapped for
     * the entire firmware-object lifetime. */
    fTxRing = OSTypeAlloc(MlxEthRing);
    if (!fTxRing || !fTxRing->init(256, 64)) {
        IOLog("MlxEthernet: TX ring allocation failed\n");
        goto fail;
    }
    fRxRing = OSTypeAlloc(MlxEthRing);
    if (!fRxRing || !fRxRing->init(256, 64)) {
        IOLog("MlxEthernet: RX ring allocation failed\n");
        goto fail;
    }

    /* Allocate DMA-coherent WQ buffers for both data paths. */
    if (fTxRing->allocBuffers() != kIOReturnSuccess) {
        IOLog("MlxEthernet: TX buffer allocation failed\n");
        goto fail;
    }
    if (fRxRing->allocBuffers() != kIOReturnSuccess) {
        IOLog("MlxEthernet: RX buffer allocation failed\n");
        goto fail;
    }

    /* Read the permanent MAC instead of publishing a synthetic address. */
    if (queryMacAddress(fMacAddr) != kIOReturnSuccess) {
        IOLog("MlxEthernet: permanent MAC query failed\n");
        goto fail;
    }

    /* Create and attach the Ethernet interface to the kernel protocol stack */
    if (!attachInterface(&fNic, true)) {
        IOLog("MlxEthernet: interface creation failed\n");
        goto fail;
    }
    fNetif = OSDynamicCast(IOEthernetInterface, fNic);

    IOLog("MlxEthernet: interface ready MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
          fMacAddr[0], fMacAddr[1], fMacAddr[2], fMacAddr[3],
          fMacAddr[4], fMacAddr[5]);
    return true;

fail:
    releaseResources();
    return false;
}

void MlxEthernet::stop(IOService *provider)
{
    /* Stop the data path first (destroy the mlx5e queues) */
    IOReturn kr = disable(fNic);
    if (kr != kIOReturnSuccess)
        IOLog("MlxEthernet: teardown failed; retaining DMA resources\n");
    releaseResources();
    super::stop(provider);
}

void MlxEthernet::releaseResources()
{
    if (fCore && fCore->getEQ() && fNotifierRegistered) {
        fCore->getEQ()->unregisterNotifier(MLX_EVENT_TYPE_COMPLETION, this);
        fCore->getEQ()->unregisterNotifier(MLX_EVENT_TYPE_PORT_STATE_CHANGE,
                                           this);
        fCore->getEQ()->synchronizeCallbacks();
        fNotifierRegistered = false;
    }
    if (fNic) {
        detachInterface(fNic, true);
        fNic->release();
        fNic = NULL;
        fNetif = NULL;
    }
    if (fSqn != MLX5E_INVALID_RESOURCE ||
        fRqn != MLX5E_INVALID_RESOURCE ||
        fTxCqn != MLX5E_INVALID_RESOURCE ||
        fRxCqn != MLX5E_INVALID_RESOURCE ||
        fTirn != MLX5E_INVALID_RESOURCE || fFlowRootActive) {
        IOLog("MlxEthernet: hardware resources quarantined\n");
        return;
    }
    if (fTxRing) {
        fTxRing->release();
        fTxRing = NULL;
    }
    if (fRxRing) {
        fRxRing->release();
        fRxRing = NULL;
    }
    if (fCore) {
        fCore->release();
        fCore = NULL;
    }
}

void MlxEthernet::free()
{
    releaseResources();
    if (fCore || fTxRing || fRxRing) {
        /* A failed firmware teardown intentionally retains the controller and
         * all DMA mappings. The core HCA teardown owns the final release. */
        IOLog("MlxEthernet: retaining quarantined controller resources\n");
        return;
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    super::free();
}

IOReturn MlxEthernet::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (!addrP)
        return kIOReturnBadArgument;
    memcpy(addrP->bytes, fMacAddr, kIOEthernetAddressSize);
    return kIOReturnSuccess;
}

IOReturn MlxEthernet::enable(IONetworkInterface *netif)
{
    (void)netif;
    IOReturn kr = kIOReturnSuccess;
    kern_return_t flowKr = kIOReturnSuccess;
    kern_return_t rxKr = kIOReturnSuccess;
    kern_return_t txKr = kIOReturnSuccess;

    IOLockLock(fLock);
    if (fEnabled) {
        IOLockUnlock(fLock);
        return kIOReturnSuccess;
    }
    if (fEnabling || fDisabling) {
        IOLockUnlock(fLock);
        return kIOReturnBusy;
    }
    if (fPd != MLX5E_INVALID_RESOURCE ||
        fTd != MLX5E_INVALID_RESOURCE ||
        fTisn != MLX5E_INVALID_RESOURCE ||
        fSqn != MLX5E_INVALID_RESOURCE ||
        fRqn != MLX5E_INVALID_RESOURCE ||
        fTirn != MLX5E_INVALID_RESOURCE ||
        fFlowTableId != MLX5E_INVALID_RESOURCE) {
        IOLockUnlock(fLock);
        return kIOReturnNotReady;
    }
    fEnabling = true;
    IOLockUnlock(fLock);

    /* Create the mlx5e TX/RX resources (See mlx5e_open_locked, en_main.c) */
    kr = allocGlobalResources();
    if (kr != kIOReturnSuccess) {
        IOLog("MlxEthernet: global resource allocation failed\n");
        goto fail;
    }
    kr = createTxResources();
    if (kr != kIOReturnSuccess) {
        IOLog("MlxEthernet: TX resource creation failed\n");
        goto fail_all;
    }
    kr = createRxResources();
    if (kr != kIOReturnSuccess) {
        IOLog("MlxEthernet: RX resource creation failed\n");
        goto fail_all;
    }

    /* Register before arming the CQs. */
    if (!fCore->getEQ()) {
        kr = kIOReturnNotReady;
        goto fail_all;
    }
    fCore->getEQ()->registerNotifier(MLX_EVENT_TYPE_COMPLETION, this);
    fCore->getEQ()->registerNotifier(MLX_EVENT_TYPE_PORT_STATE_CHANGE, this);
    fNotifierRegistered = true;
    armCq(fTxCqn, fTxCqDbOffset, fTxArmSn, fTxCqCc);
    armCq(fRxCqn, fRxCqDbOffset, fRxArmSn, fRxCqCc);

    kr = createRxFlowSteering();
    if (kr != kIOReturnSuccess) {
        IOLog("MlxEthernet: RX flow steering creation failed\n");
        goto fail_all;
    }

    IOLockLock(fLock);
    fEnabled = true;
    fEnabling = false;
    IOLockUnlock(fLock);
    setLinkState(queryLinkState(), 0);
    return kIOReturnSuccess;

fail_all:
    if (fNotifierRegistered && fCore->getEQ()) {
        fCore->getEQ()->unregisterNotifier(MLX_EVENT_TYPE_COMPLETION, this);
        fCore->getEQ()->unregisterNotifier(MLX_EVENT_TYPE_PORT_STATE_CHANGE,
                                           this);
        fCore->getEQ()->synchronizeCallbacks();
        fNotifierRegistered = false;
    }
    flowKr = destroyRxFlowSteering();
    rxKr = flowKr == kIOReturnSuccess ?
        destroyRxResources() : flowKr;
    txKr = destroyTxResources();
    if (flowKr == kIOReturnSuccess && rxKr == kIOReturnSuccess &&
        txKr == kIOReturnSuccess)
        deallocGlobalResources();
fail:
    IOLockLock(fLock);
    fEnabled = false;
    fEnabling = false;
    IOLockUnlock(fLock);
    setLinkState(false, 0);
    return kr;
}

IOReturn MlxEthernet::disable(IONetworkInterface *netif)
{
    (void)netif;
    IOLockLock(fLock);
    if (fEnabling || fDisabling) {
        IOLockUnlock(fLock);
        return kIOReturnBusy;
    }
    fDisabling = true;
    fEnabled = false;
    IOLockUnlock(fLock);
    setLinkState(false, 0);
    if (fNotifierRegistered && fCore && fCore->getEQ()) {
        fCore->getEQ()->unregisterNotifier(MLX_EVENT_TYPE_COMPLETION, this);
        fCore->getEQ()->unregisterNotifier(MLX_EVENT_TYPE_PORT_STATE_CHANGE,
                                           this);
        fCore->getEQ()->synchronizeCallbacks();
        fNotifierRegistered = false;
    }
    kern_return_t flowKr = destroyRxFlowSteering();
    kern_return_t rxKr = flowKr == kIOReturnSuccess ?
        destroyRxResources() : flowKr;
    kern_return_t txKr = destroyTxResources();
    kern_return_t result = flowKr;
    if (result == kIOReturnSuccess)
        result = rxKr;
    if (result == kIOReturnSuccess)
        result = txKr;
    if (result == kIOReturnSuccess)
        result = deallocGlobalResources();
    IOLockLock(fLock);
    fDisabling = false;
    IOLockUnlock(fLock);
    return result;
}

IOReturn MlxEthernet::setPromiscuousMode(bool active)
{
    (void)active;
    return kIOReturnUnsupported;
}

IOReturn MlxEthernet::setMulticastMode(bool active)
{
    (void)active;
    return kIOReturnUnsupported;
}

IOReturn MlxEthernet::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    if (group == gIONetworkFilterGroup) {
        *filters = kIOPacketFilterUnicast | kIOPacketFilterMulticast |
                   kIOPacketFilterBroadcast;
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

IOReturn MlxEthernet::getMaxPacketSize(UInt32 *maxSize) const
{
    *maxSize = 1518;
    return kIOReturnSuccess;
}

IOReturn MlxEthernet::getMinPacketSize(UInt32 *minSize) const
{
    *minSize = 60;
    return kIOReturnSuccess;
}

IOReturn MlxEthernet::setProperties(OSObject *properties)
{
    return kIOReturnSuccess;
}

/* TX: see mlx5e_xmit (en_tx.c:666) */
UInt32 MlxEthernet::outputPacket(mbuf_t packet, void *param)
{
    (void)param;
    if (!fEnabled || !packet || !fTxRing ||
        fSqn == MLX5E_INVALID_RESOURCE)
        return kIOReturnOutputStall;

    if (xmitPacket(packet) != kIOReturnSuccess)
        return kIOReturnOutputStall;
    return kIOReturnOutputSuccess;
}

kern_return_t MlxEthernet::xmitPacket(mbuf_t packet)
{
    /* See mlx5e_xmit (en_tx.c:666) + mlx5e_sq_xmit_wqe:
     * WQE = ctrl(16B) + eth(16B) + data seg (DMA)
     * Write to the SQ ring buffer → update the DB record → ring the doorbell */
    if (!packet || !fTxRing || !fTxRing->getWqBuf() ||
        fSqn == MLX5E_INVALID_RESOURCE || !fTxBufDesc || !fTxBufDMA ||
        !fLkey || !fTxBfValid)
        return kIOReturnInvalid;

    uint32_t len = mbuf_pkthdr_len(packet);
    if (len == 0) {
        mbuf_freem(packet);
        return kIOReturnSuccess;
    }
    if (len > MLX5E_TX_BUF_SIZE) {
        mbuf_freem(packet);
        return kIOReturnSuccess;
    }

    IOLockLock(fLock);
    if (!fEnabled || fSqn == MLX5E_INVALID_RESOURCE || !fTxBfValid) {
        IOLockUnlock(fLock);
        return kIOReturnNotReady;
    }
    /* Check ring room (See mlx5e_wqc_has_room_for, txrx.h) */
    if ((uint16_t)(fTxPc - fTxCc) >= fTxRing->getSize()) {
        IOLockUnlock(fLock);
        return kIOReturnOutputStall;
    }

    /* Get an SQ slot */
    uint16_t pi = fTxPc & (fTxRing->getSize() - 1);

    /* WQE: ctrl + eth + data seg (see mlx5e_tx_wqe, en.h:244) */
    MlxEthTxWqe *wqe = (MlxEthTxWqe *)fTxRing->getWqebb(pi);
    memset(wqe, 0, 64);

    /* ctrl segment (see mlx5e_txwqe_complete, en_tx.c)
     *   opmod_idx_opcode = (pc << 8) | opcode
     *   qpn_ds           = (sqn << 8) | ds_cnt   (ds in 16-byte units) */
    wqe->ctrl.opmod_idx_opcode = OSSwapHostToBigInt32(
        ((uint32_t)fTxPc << 8) | MLX_OPCODE_SEND);
    /* Copy the complete mbuf chain into a persistent DMA bounce slot. */
    uint8_t *data = static_cast<uint8_t *>(fTxBufDesc->getBytesNoCopy()) +
                    ((uint32_t)pi * MLX5E_TX_BUF_SIZE);
    if (mbuf_copydata(packet, 0, len, data) != 0) {
        IOLockUnlock(fLock);
        mbuf_freem(packet);
        return kIOReturnSuccess;
    }

    /* The vport may require the L2 header inline. The first two inline bytes
     * live in the Ethernet segment and the remaining bytes occupy one DS. */
    uint32_t inlineLen = fTxMinInlineMode == 1 ? 14 : 0;
    if (inlineLen && len < inlineLen) {
        IOLockUnlock(fLock);
        mbuf_freem(packet);
        return kIOReturnSuccess;
    }
    MlxWqeDataSeg *dseg;
    uint32_t dsCount;
    if (inlineLen) {
        wqe->eth.inline_hdr.sz = OSSwapHostToBigInt16(inlineLen);
        memcpy(wqe->eth.inline_hdr.start, data, inlineLen);
        dseg = reinterpret_cast<MlxWqeDataSeg *>(
            static_cast<uint8_t *>(fTxRing->getWqebb(pi)) + 48);
        dsCount = 4;
    } else {
        dseg = &wqe->data[0];
        dsCount = 3;
    }
    wqe->ctrl.qpn_ds = OSSwapHostToBigInt32((fSqn << 8) | dsCount);
    wqe->eth.cs_flags = 0;
    dseg->byte_count = OSSwapHostToBigInt32(len - inlineLen);
    dseg->lkey = OSSwapHostToBigInt32(fLkey);
    dseg->addr = OSSwapHostToBigInt64(
        fTxBufDMA + (uint64_t)pi * MLX5E_TX_BUF_SIZE + inlineLen);

    /* Keep the mbuf alive until the TX CQE arrives (See mlx5e_tx_wqe_info.skb) */
    fTxRing->setHead(fTxPc);
    fTxRing->setWqeMbuf(pi, packet);
    fTxPc++;

    /* Notify the hardware (See mlx5e_notify_hw, txrx.h):
     *   1. set CQ_UPDATE, write the DB record
     *   2. 64-bit blue-flame doorbell write to the UAR */
    MlxEthTxWqe *w = (MlxEthTxWqe *)fTxRing->getWqebb(pi);
    w->ctrl.fm_ce_se = MLX_WQE_CTRL_CQ_UPDATE;
    mlxMemoryBarrier();
    if (fTxRing->getDbRecord())
        *fTxRing->getDbRecord() = OSSwapHostToBigInt32(fTxPc);
    /* mlx5_write64(ctrl, uar_map): write the first 8 bytes of the control
     * segment as one ordered PCI MMIO doorbell. */
    volatile uint64_t *bf = static_cast<volatile uint64_t *>(fTxBf.map);
    *bf = *reinterpret_cast<volatile uint64_t *>(&w->ctrl);
    OSSynchronizeIO();
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::xmitInline(mbuf_t packet)
{
    (void)packet;
    return kIOReturnUnsupported;
}

kern_return_t MlxEthernet::xmitDma(mbuf_t packet)
{
    (void)packet;
    return kIOReturnUnsupported;
}

/* RX: see mlx5e_handle_rx_cqe (en_rx.c:70) */
void MlxEthernet::handleRxCqe(struct MlxCqe64 *cqe)
{
    /* A received packet arrived on the RX RQ. See mlx5e_handle_rx_cqe:
     *   byte_cnt = packet length (BE), cqe->sop_drop_qpn bit31 = start of packet */
    if (!cqe || !fRxBufDesc || !fRxBufDMA)
        return;
    uint16_t wqe_counter = OSSwapBigToHostInt16(cqe->wqe_counter);
    uint16_t wqe_idx = wqe_counter &
        ((1u << MLX5E_DEFAULT_LOG_RQ_SIZE) - 1);
    uint32_t bcnt = OSSwapBigToHostInt32(cqe->byte_cnt);
    uint8_t opcode = MLX_CQE_GET_OPCODE(cqe);

    /* The packet data was DMA'd into the RX buffer pool slot. Allocate an
     * mbuf, copy the payload, and hand it to the kernel protocol stack. */
    mbuf_t m = NULL;
    uint8_t *src = NULL;
    if (opcode == MLX_CQE_RESP && bcnt > 0 && bcnt <= MLX5E_RX_BUF_SIZE) {
        src = static_cast<uint8_t *>(fRxBufDesc->getBytesNoCopy()) +
              ((uint32_t)wqe_idx * MLX5E_RX_BUF_SIZE);
        mbuf_allocpacket(MBUF_DONTWAIT, bcnt, NULL, &m);
    }
    if (m) {
        memcpy(mbuf_data(m), src, bcnt);
        mbuf_setlen(m, bcnt);
        mbuf_pkthdr_setlen(m, bcnt);
    }
    /* Every consumed CQE returns one cyclic RQ slot, including errors. */
    uint16_t post_idx = fRxPc &
        ((1u << MLX5E_DEFAULT_LOG_RQ_SIZE) - 1);
    if (postRxWqe(post_idx) == kIOReturnSuccess) {
        fRxPc++;
        mlxMemoryBarrier();
        *fRxRing->getDbRecord() = OSSwapHostToBigInt32(fRxPc);
    }
    fRxCc = static_cast<uint16_t>(wqe_counter + 1);
    if (m)
        receivePacket(m, bcnt);
}

kern_return_t MlxEthernet::receivePacket(mbuf_t packet, UInt32 length)
{
    /* After receiving a packet from the hardware RX ring, hand it to the kernel protocol stack */
    if (!fEnabled || !packet) {
        if (packet) mbuf_freem(packet);
        return kIOReturnNotReady;
    }
    /* See Linux netif_receive_skb → macOS: fNic->inputPacket */
    if (fNic)
        fNic->inputPacket(packet, length, 0);
    else
        mbuf_freem(packet);
    return kIOReturnSuccess;
}

void MlxEthernet::setLinkState(bool up, UInt32 speedMbps)
{
    IOLockLock(fLock);
    fLinkUp = up;
    fLinkSpeed = speedMbps;
    IOLockUnlock(fLock);

    /* Report to the kernel */
    setLinkStatus(up ? (kIONetworkLinkValid | kIONetworkLinkActive) : 0,
                  NULL, (UInt64)speedMbps * 1000000ULL);
    IOLog("MlxEthernet: link %s (%u Mbps)\n",
          up ? "UP" : "DOWN", speedMbps);
}

/* ========== mlx5e resource creation (See en_main.c / transobj.c) ========== */

/* Helper: parse the object id from a create_*_out (24 bits at bit 0x48) */
static uint32_t
mlx5e_out_obj(void *out)
{
    return static_cast<uint32_t>(mlxGetBits(out, 0x48, 24));
}

kern_return_t MlxEthernet::cmdAllocTd(uint32_t *td)
{
    /* ALLOC_TRANSPORT_DOMAIN (0x816) — output: transport_domain at bit 0x48 */
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_ALLOC_TRANSPORT_DOMAIN);
    kern_return_t kr = fCore->exec(MLX_CMD_OP_ALLOC_TRANSPORT_DOMAIN,
                                   in, sizeof(in), out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess && td)
        *td = mlx5e_out_obj(out);
    return kr;
}

kern_return_t MlxEthernet::cmdAllocPd(uint32_t *pd)
{
    /* ALLOC_PD (0x800) — output: pd at bit 0x48 */
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_ALLOC_PD);
    kern_return_t kr = fCore->exec(MLX_CMD_OP_ALLOC_PD,
                                   in, sizeof(in), out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess && pd)
        *pd = mlx5e_out_obj(out);
    return kr;
}

kern_return_t MlxEthernet::cmdCreateMkey(uint32_t pd, uint32_t *mkeyIndex,
                                         uint32_t *lkey)
{
    /* mlx5e uses a PA-mode local read/write MKey for kernel DMA addresses. */
    uint8_t in[0x110] = {};
    uint8_t out[16] = {};
    const uint8_t variant = 0x42;
    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_MKEY);
    uint8_t *mkc = in + 0x10;                /* MKC starts at bit 0x80 */
    mlxSetBits(mkc, 0x14, 1, 1);             /* local write */
    mlxSetBits(mkc, 0x15, 1, 1);             /* local read */
    mlxSetBits(mkc, 0x16, 2, 0);             /* PA access mode */
    mlxSetBits(mkc, 0x20, 24, 0xffffff);      /* unrestricted QPN */
    mlxSetBits(mkc, 0x38, 8, variant);
    mlxSetBits(mkc, 0x60, 1, 1);             /* length64 */
    mlxSetBits(mkc, 0x68, 24, pd);
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_MKEY, in, sizeof(in),
                                   out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) {
        uint32_t index = mlx5e_out_obj(out);
        if (mkeyIndex) *mkeyIndex = index;
        if (lkey) *lkey = (index << 8) | variant;
    }
    return kr;
}

kern_return_t MlxEthernet::cmdDestroyObject(uint32_t opcode,
                                             uint32_t objectId)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, static_cast<uint16_t>(opcode));
    mlxSetBits(in, 0x48, 24, objectId);
    return fCore->exec(opcode, in, sizeof(in), out, sizeof(out), 5000);
}

kern_return_t MlxEthernet::cmdCreateTis(uint32_t td, uint32_t pd, uint32_t *tisn)
{
    /* CREATE_TIS (0x912). tisc at bit 0x100:
     *   transport_domain at +0x128 (24b), pd at +0x168 (24b) */
    uint8_t in[0xc0] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_TIS);
    mlxSetBits(in, 0x100 + 0x128, 24, td);
    mlxSetBits(in, 0x100 + 0x168, 24, pd);
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_TIS,
                                   in, sizeof(in), out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess && tisn)
        *tisn = mlx5e_out_obj(out);
    return kr;
}

kern_return_t MlxEthernet::cmdCreateCq(uint32_t logSize, uint32_t *cqn,
                                        void **cqBuf,
                                        IOBufferMemoryDescriptor **cqDesc,
                                        IODMACommand **cqMap, uint64_t *cqDMA,
                                        uint32_t *dbOffset)
{
    /* CREATE_CQ (0x400). cqc at bit 0x80 (byte 0x10):
     *   cqe_sz at +0x8 (3b, 0=64B), log_cq_size at +0x63 (5b),
     *   uar_page at +0x68 (24b), c_eqn at +0xb8 (8b),
     *   log_page_size at +0xc3 (5b), dbr_addr at +0x1c0 (64b)
     * cq_umem_valid at input bit 0x2e0, pas at input bit 0x880 */
    uint32_t cqeBytes = (1u << logSize) * sizeof(MlxCqe64);
    IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, cqeBytes, 0xFFFFFFF000ULL);
    if (!desc)
        return kIOReturnNoMemory;
    memset(desc->getBytesNoCopy(), 0, cqeBytes);
    MlxCqe64 *cqes = reinterpret_cast<MlxCqe64 *>(desc->getBytesNoCopy());
    for (uint32_t i = 0; i < (1u << logSize); i++)
        cqes[i].op_own = 0xf1;

    IODMACommand *map = NULL;
    uint64_t dma = 0;
    if (mlxMapDMAContiguous(desc, &map, &dma) != kIOReturnSuccess) {
        desc->release();
        return kIOReturnNoMemory;
    }

    /* Doorbell record slot in the shared UAR DB page */
    uint32_t dbOff = 0;
    if (fCore->getUAR()->allocDbSlots(2, &dbOff) != kIOReturnSuccess) {
        mlxUnmapDMA(map);
        desc->release();
        return kIOReturnNoResources;
    }

    uint8_t in[0x1000] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_CQ);
    uint8_t *cqc = in + 0x10;              /* cqc at byte 0x10 */
    mlxSetBits(cqc, 0x08, 3, 0);           /* 64-byte CQE */
    mlxSetBits(cqc, 0x63, 5, logSize);
    mlxSetBits(cqc, 0x68, 24, fCore->getUAR()->getBootUarIndex());
    mlxSetBits(cqc, 0xb8, 8, fCore->getEQ()->getCompEqNumber(0));
    mlxSetBits(cqc, 0xc3, 5, 0);           /* 4K pages */
    mlxSetBits(cqc, 0x1c0, 64, fCore->getUAR()->getDbRecordDMA() + dbOff);
    for (uint32_t i = 0; i < (cqeBytes + 4095) / 4096; i++)
        OSWriteBigInt64(in, 0x880 / 8 + i * 8, dma + (uint64_t)i * 4096);

    uint32_t numPages = (cqeBytes + 4095) / 4096;
    uint32_t inSize = 0x880 / 8 + numPages * 8;
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_CQ, in, inSize,
                                   out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        fCore->getUAR()->freeDbSlots(dbOff, 2);
        mlxUnmapDMA(map);
        desc->release();
        return kr;
    }

    if (cqn)     *cqn = mlx5e_out_obj(out);
    if (cqBuf)   *cqBuf = desc->getBytesNoCopy();
    if (cqDesc)  *cqDesc = desc;      /* transfer ownership to the caller */
    else         desc->release();
    if (cqMap)   *cqMap = map;
    if (cqDMA)   *cqDMA = dma;
    if (dbOffset) *dbOffset = dbOff;
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::cmdDestroyCq(uint32_t cqn)
{
    return cmdDestroyObject(MLX_CMD_OP_DESTROY_CQ, cqn);
}

kern_return_t MlxEthernet::cmdCreateSq(uint32_t tisn, uint32_t cqn, uint32_t *sqn,
                                       uint32_t *dbOffset)
{
    /* CREATE_SQ (0x904). sqc at bit 0x100:
     *   state at +0x8 (4b, RST=0), cqn at +0x48 (24b),
     *   tis_lst_sz at +0x100 (16b), tis_num_0 at +0x168 (24b)
     * wq at +0x180:
     *   wq_type at +0x0 (4b, CYCLIC=1), pd at +0x48 (24b),
     *   uar_page at +0x68 (24b), dbr_addr at +0x80 (64b),
     *   log_wq_stride at +0x10c (4b), log_wq_pg_sz at +0x113 (5b),
     *   log_wq_sz at +0x11b (5b), dbr_umem_valid at +0x120,
     *   wq_umem_valid at +0x121, pas at +0x600
     * The WQ buffer is the TX ring buffer (already DMA-coherent). */
    uint32_t logSize = MLX5E_DEFAULT_LOG_SQ_SIZE;
    if (!fTxRing || !fTxRing->getWqBuf() || !fTxRing->getWqDMA())
        return kIOReturnNotReady;

    uint32_t dbOff = 0;
    if (fCore->getUAR()->allocDbSlots(2, &dbOff) != kIOReturnSuccess)
        return kIOReturnNoResources;

    uint8_t in[0x1000] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_SQ);
    uint8_t *sqc = in + 0x20;              /* sqc at byte 0x20 */
    mlxSetBits(sqc, 0x08, 4, 0);           /* state RST */
    mlxSetBits(sqc, 0x05, 3, fTxMinInlineMode);
    mlxSetBits(sqc, 0x48, 24, cqn);
    mlxSetBits(sqc, 0x100, 16, 1);         /* tis_lst_sz = 1 */
    mlxSetBits(sqc, 0x168, 24, tisn);
    uint8_t *wq = sqc + 0x30;              /* wq at byte 0x50 */
    mlxSetBits(wq, 0x00, 4, 1);            /* wq_type CYCLIC */
    mlxSetBits(wq, 0x48, 24, fPd);
    mlxSetBits(wq, 0x68, 24, fCore->getUAR()->getBootUarIndex());
    mlxSetBits(wq, 0x80, 64, fCore->getUAR()->getDbRecordDMA() + dbOff);
    mlxSetBits(wq, 0x10c, 4, 6);           /* log_wq_stride = 64B */
    mlxSetBits(wq, 0x113, 5, 0);           /* 4K pages */
    mlxSetBits(wq, 0x11b, 5, logSize);
    /* PAS at wq bit 0x600 = input byte 0x50 + 0xc0 = 0x110 */
    for (uint32_t i = 0; i < 4; i++)
        OSWriteBigInt64(in, 0x110 + i * 8,
                        fTxRing->getWqDMA() + (uint64_t)i * 4096);

    uint32_t inSize = 0x110 + 4 * 8;
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_SQ, in, inSize,
                                   out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        fCore->getUAR()->freeDbSlots(dbOff, 2);
        return kr;
    }

    if (sqn)     *sqn = mlx5e_out_obj(out);
    if (dbOffset) *dbOffset = dbOff;

    /* Point the ring doorbell record at the SQ DB slot */
    fTxRing->setDbRecord(fCore->getUAR()->getDbRecord() + dbOff / 4 +
                         MLX_SND_DBR);
    fTxRing->setDbDMA(fCore->getUAR()->getDbRecordDMA() + dbOff);
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::cmdCreateRq(uint32_t cqn, uint32_t *rqn,
                                       uint32_t *dbOffset)
{
    /* CREATE_RQ (0x908). rqc at bit 0x100:
     *   mem_rq_type at +0x4 (4b, INLINE=0), state at +0x8 (4b, RST=0),
     *   cqn at +0x48 (24b), counter_set_id at +0x60 (8b)
     * wq at +0x180 (same layout as SQ)
     * The WQ buffer is the RX ring buffer (already DMA-coherent). */
    uint32_t logSize = MLX5E_DEFAULT_LOG_RQ_SIZE;
    if (!fRxRing || !fRxRing->getWqBuf() || !fRxRing->getWqDMA())
        return kIOReturnNotReady;

    uint32_t dbOff = 0;
    if (fCore->getUAR()->allocDbSlots(2, &dbOff) != kIOReturnSuccess)
        return kIOReturnNoResources;

    uint8_t in[0x1000] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_RQ);
    uint8_t *rqc = in + 0x20;              /* rqc at byte 0x20 */
    mlxSetBits(rqc, 0x04, 4, 0);           /* mem_rq_type INLINE */
    mlxSetBits(rqc, 0x08, 4, 0);           /* state RST */
    mlxSetBits(rqc, 0x48, 24, cqn);
    uint8_t *wq = rqc + 0x30;              /* wq at byte 0x50 */
    mlxSetBits(wq, 0x00, 4, 1);            /* wq_type CYCLIC */
    mlxSetBits(wq, 0x48, 24, fPd);
    mlxSetBits(wq, 0x68, 24, fCore->getUAR()->getBootUarIndex());
    mlxSetBits(wq, 0x80, 64, fCore->getUAR()->getDbRecordDMA() + dbOff);
    mlxSetBits(wq, 0x10c, 4, 6);           /* log_wq_stride = 64B */
    mlxSetBits(wq, 0x113, 5, 0);           /* 4K pages */
    mlxSetBits(wq, 0x11b, 5, logSize);
    for (uint32_t i = 0; i < 4; i++)
        OSWriteBigInt64(in, 0x110 + i * 8,
                        fRxRing->getWqDMA() + (uint64_t)i * 4096);

    uint32_t inSize = 0x110 + 4 * 8;
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_RQ, in, inSize,
                                   out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        fCore->getUAR()->freeDbSlots(dbOff, 2);
        return kr;
    }

    if (rqn)     *rqn = mlx5e_out_obj(out);
    if (dbOffset) *dbOffset = dbOff;

    fRxRing->setDbRecord(fCore->getUAR()->getDbRecord() + dbOff / 4);
    fRxRing->setDbDMA(fCore->getUAR()->getDbRecordDMA() + dbOff);
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::cmdCreateTir(uint32_t rqn, uint32_t td, uint32_t *tirn)
{
    /* CREATE_TIR (0x900). tirc at bit 0x100:
     *   disp_type at +0x20 (4b, DIRECT=0), inline_rqn at +0xe8 (24b),
     *   transport_domain at +0x128 (24b) */
    uint8_t in[0x100] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_CREATE_TIR);
    mlxSetBits(in, 0x100 + 0x20, 4, 0);      /* DIRECT RQ */
    mlxSetBits(in, 0x100 + 0xe8, 24, rqn);
    mlxSetBits(in, 0x100 + 0x128, 24, td);
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_TIR,
                                   in, sizeof(in), out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess && tirn)
        *tirn = mlx5e_out_obj(out);
    return kr;
}

kern_return_t MlxEthernet::cmdModifySqReady(uint32_t sqn)
{
    uint8_t in[0x110] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_MODIFY_SQ);
    mlxSetBits(in, 0x40, 4, 0);             /* current state RST */
    mlxSetBits(in, 0x48, 24, sqn);
    mlxSetBits(in, 0x108, 4, 1);            /* new state RDY */
    return fCore->exec(MLX_CMD_OP_MODIFY_SQ, in, sizeof(in),
                       out, sizeof(out), 5000);
}

kern_return_t MlxEthernet::cmdModifyRqReady(uint32_t rqn)
{
    uint8_t in[0x110] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_MODIFY_RQ);
    mlxSetBits(in, 0x40, 4, 0);             /* current state RST */
    mlxSetBits(in, 0x48, 24, rqn);
    mlxSetBits(in, 0x108, 4, 1);            /* new state RDY */
    return fCore->exec(MLX_CMD_OP_MODIFY_RQ, in, sizeof(in),
                       out, sizeof(out), 5000);
}

kern_return_t MlxEthernet::queryMacAddress(uint8_t mac[6])
{
    if (!mac)
        return kIOReturnBadArgument;
    uint8_t in[16] = {};
    uint8_t out[0x110] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
    kern_return_t kr = fCore->exec(MLX_CMD_OP_QUERY_NIC_VPORT_CONTEXT,
                                   in, sizeof(in), out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        return kr;
    fTxMinInlineMode = static_cast<uint8_t>(mlxGetBits(out, 0x85, 3));
    if (fTxMinInlineMode > 1) {
        IOLog("MlxEthernet: unsupported minimum inline mode %u\n",
              fTxMinInlineMode);
        return kIOReturnUnsupported;
    }
    memcpy(mac, out + 0x106, 6);
    bool allZero = true;
    for (uint32_t i = 0; i < 6; i++)
        allZero &= mac[i] == 0;
    return allZero ? kIOReturnNotFound : kIOReturnSuccess;
}

bool MlxEthernet::queryLinkState()
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_QUERY_VPORT_STATE);
    mlxSetBits(in, 0x30, 16, 0);             /* VNIC vport */
    if (fCore->exec(MLX_CMD_OP_QUERY_VPORT_STATE, in, sizeof(in),
                    out, sizeof(out), 5000) != kIOReturnSuccess)
        return false;
    return mlxGetBits(out, 0x7c, 4) == 1;
}

kern_return_t MlxEthernet::createRxFlowSteering()
{
    uint8_t out[16] = {};
    uint8_t tableIn[0x38] = {};
    uint8_t groupIn[0x400] = {};
    uint8_t fteIn[0x348] = {};
    uint8_t rootIn[0x40] = {};
    kern_return_t kr;

    /* One-entry NIC_RX catch-all table. The root is connected last. */
    OSWriteBigInt16(tableIn, 0, MLX_CMD_OP_CREATE_FLOW_TABLE);
    mlxSetBits(tableIn, 0x80, 8, 0);         /* NIC_RX */
    mlxSetBits(tableIn, 0xc8, 8, 0);         /* level 0 */
    mlxSetBits(tableIn, 0xd8, 8, 0);         /* one entry */
    kr = fCore->exec(MLX_CMD_OP_CREATE_FLOW_TABLE, tableIn,
                     sizeof(tableIn), out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        return kr;
    fFlowTableId = mlx5e_out_obj(out);

    memset(out, 0, sizeof(out));
    OSWriteBigInt16(groupIn, 0, MLX_CMD_OP_CREATE_FLOW_GROUP);
    mlxSetBits(groupIn, 0x80, 8, 0);
    mlxSetBits(groupIn, 0xa8, 24, fFlowTableId);
    mlxSetBits(groupIn, 0xe0, 32, 0);
    mlxSetBits(groupIn, 0x120, 32, 0);
    mlxSetBits(groupIn, 0x1f8, 8, 0);        /* catch-all */
    kr = fCore->exec(MLX_CMD_OP_CREATE_FLOW_GROUP, groupIn,
                     sizeof(groupIn), out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        goto fail;
    fFlowGroupId = mlx5e_out_obj(out);

    memset(out, 0, sizeof(out));
    OSWriteBigInt16(fteIn, 0, MLX_CMD_OP_SET_FLOW_TABLE_ENTRY);
    mlxSetBits(fteIn, 0x80, 8, 0);
    mlxSetBits(fteIn, 0xa8, 24, fFlowTableId);
    mlxSetBits(fteIn, 0x100, 32, 0);         /* flow index */
    mlxSetBits(fteIn, 0x220, 32, fFlowGroupId);
    mlxSetBits(fteIn, 0x270, 16, 0x4);       /* forward destination */
    mlxSetBits(fteIn, 0x288, 24, 1);
    mlxSetBits(fteIn, 0x1a00, 8, 2);         /* TIR */
    mlxSetBits(fteIn, 0x1a08, 24, fTirn);
    kr = fCore->exec(MLX_CMD_OP_SET_FLOW_TABLE_ENTRY, fteIn,
                     sizeof(fteIn), out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        goto fail;
    fFlowEntryValid = true;

    memset(out, 0, sizeof(out));
    OSWriteBigInt16(rootIn, 0, MLX_CMD_OP_SET_FLOW_TABLE_ROOT);
    mlxSetBits(rootIn, 0x80, 8, 0);
    mlxSetBits(rootIn, 0xa8, 24, fFlowTableId);
    kr = fCore->exec(MLX_CMD_OP_SET_FLOW_TABLE_ROOT, rootIn,
                     sizeof(rootIn), out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        goto fail;
    fFlowRootActive = true;
    return kIOReturnSuccess;

fail:
    destroyRxFlowSteering();
    return kr;
}

kern_return_t MlxEthernet::destroyRxFlowSteering()
{
    uint8_t in[0x40] = {};
    uint8_t out[16] = {};
    kern_return_t kr;
    if (fFlowRootActive) {
        OSWriteBigInt16(in, 0, MLX_CMD_OP_SET_FLOW_TABLE_ROOT);
        mlxSetBits(in, 0x30, 16, 1);         /* disconnect root */
        mlxSetBits(in, 0x80, 8, 0);
        kr = fCore->exec(MLX_CMD_OP_SET_FLOW_TABLE_ROOT, in, sizeof(in),
                         out, sizeof(out), 5000);
        if (kr != kIOReturnSuccess)
            return kr;
        fFlowRootActive = false;
    }
    if (fFlowEntryValid) {
        memset(in, 0, sizeof(in));
        memset(out, 0, sizeof(out));
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DELETE_FLOW_TABLE_ENTRY);
        mlxSetBits(in, 0x80, 8, 0);
        mlxSetBits(in, 0xa8, 24, fFlowTableId);
        mlxSetBits(in, 0x100, 32, 0);
        kr = fCore->exec(MLX_CMD_OP_DELETE_FLOW_TABLE_ENTRY, in,
                         sizeof(in), out, sizeof(out), 5000);
        if (kr != kIOReturnSuccess)
            return kr;
        fFlowEntryValid = false;
    }
    if (fFlowGroupId != MLX5E_INVALID_RESOURCE) {
        memset(in, 0, sizeof(in));
        memset(out, 0, sizeof(out));
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_FLOW_GROUP);
        mlxSetBits(in, 0x80, 8, 0);
        mlxSetBits(in, 0xa8, 24, fFlowTableId);
        mlxSetBits(in, 0xc0, 32, fFlowGroupId);
        kr = fCore->exec(MLX_CMD_OP_DESTROY_FLOW_GROUP, in, sizeof(in),
                         out, sizeof(out), 5000);
        if (kr != kIOReturnSuccess)
            return kr;
        fFlowGroupId = MLX5E_INVALID_RESOURCE;
    }
    if (fFlowTableId != MLX5E_INVALID_RESOURCE) {
        memset(in, 0, sizeof(in));
        memset(out, 0, sizeof(out));
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_FLOW_TABLE);
        mlxSetBits(in, 0x80, 8, 0);
        mlxSetBits(in, 0xa8, 24, fFlowTableId);
        kr = fCore->exec(MLX_CMD_OP_DESTROY_FLOW_TABLE, in, sizeof(in),
                         out, sizeof(out), 5000);
        if (kr != kIOReturnSuccess)
            return kr;
        fFlowTableId = MLX5E_INVALID_RESOURCE;
    }
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::allocGlobalResources()
{
    kern_return_t kr = cmdAllocPd(&fPd);
    if (kr != kIOReturnSuccess)
        return kr;
    kr = cmdAllocTd(&fTd);
    if (kr != kIOReturnSuccess)
        goto fail;
    kr = cmdCreateMkey(fPd, &fMkeyIndex, &fLkey);
    if (kr != kIOReturnSuccess)
        goto fail;
    return kIOReturnSuccess;

fail:
    deallocGlobalResources();
    return kr;
}

kern_return_t MlxEthernet::deallocGlobalResources()
{
    kern_return_t firstError = kIOReturnSuccess;
    kern_return_t kr;
    if (fMkeyIndex != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyObject(MLX_CMD_OP_DESTROY_MKEY, fMkeyIndex);
        if (kr != kIOReturnSuccess)
            return kr;
        fMkeyIndex = MLX5E_INVALID_RESOURCE;
        fLkey = 0;
    }
    if (fTd != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyObject(MLX_CMD_OP_DEALLOC_TRANSPORT_DOMAIN, fTd);
        if (kr != kIOReturnSuccess)
            firstError = kr;
        else
            fTd = MLX5E_INVALID_RESOURCE;
    }
    if (firstError == kIOReturnSuccess && fPd != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyObject(MLX_CMD_OP_DEALLOC_PD, fPd);
        if (kr != kIOReturnSuccess)
            firstError = kr;
        else
            fPd = MLX5E_INVALID_RESOURCE;
    }
    return firstError;
}

kern_return_t MlxEthernet::createTxResources()
{
    /* TX uses a persistent bounce pool so arbitrary mbuf chains are copied
     * into one DMA segment with a lifetime matching the SQ. */
    fTxPc = 0;
    fTxCc = 0;
    fTxCqCc = 0;
    fTxArmSn = 0;
    for (uint16_t i = 0; i < (1u << MLX5E_DEFAULT_LOG_SQ_SIZE); i++)
        fTxRing->clearWqeMbuf(i);

    uint32_t poolBytes = (1u << MLX5E_DEFAULT_LOG_SQ_SIZE) * MLX5E_TX_BUF_SIZE;
    fTxBufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, poolBytes, 0xFFFFFFF000ULL);
    if (!fTxBufDesc)
        return kIOReturnNoMemory;
    memset(fTxBufDesc->getBytesNoCopy(), 0, poolBytes);
    kern_return_t kr = mlxMapDMAContiguous(fTxBufDesc, &fTxBufMap, &fTxBufDMA);
    if (kr != kIOReturnSuccess)
        goto fail;

    kr = cmdCreateTis(fTd, fPd, &fTisn);
    if (kr != kIOReturnSuccess)
        goto fail;
    kr = cmdCreateCq(MLX5E_DEFAULT_LOG_SQ_SIZE, &fTxCqn, &fTxCqBuf,
                      &fTxCqDesc, &fTxCqMap, &fTxCqDMA, &fTxCqDbOffset);
    if (kr != kIOReturnSuccess)
        goto fail;
    kr = cmdCreateSq(fTisn, fTxCqn, &fSqn, &fTxDbOffset);
    if (kr != kIOReturnSuccess)
        goto fail;
    /* Allocate a blue-flame doorbell register (See mlx5_alloc_bfreg) */
    kr = fCore->getUAR()->allocBfreg(&fTxBf);
    if (kr != kIOReturnSuccess)
        goto fail;
    fTxBfValid = true;
    kr = cmdModifySqReady(fSqn);
    if (kr != kIOReturnSuccess)
        goto fail;
    return kIOReturnSuccess;

fail:
    destroyTxResources();
    return kr;
}

kern_return_t MlxEthernet::createRxResources()
{
    /* Prepare all RX DMA memory and WQEs before transitioning the RQ to RDY. */
    fRxPc = 0;
    fRxCc = 0;
    fRxCqCc = 0;
    fRxArmSn = 0;
    uint32_t poolBytes = MLX5E_MAX_RX_BUFS * MLX5E_RX_BUF_SIZE;
    fRxBufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, poolBytes, 0xFFFFFFF000ULL);
    if (!fRxBufDesc)
        return kIOReturnNoMemory;
    memset(fRxBufDesc->getBytesNoCopy(), 0, poolBytes);
    kern_return_t kr = mlxMapDMAContiguous(fRxBufDesc, &fRxBufMap,
                                           &fRxBufDMA);
    if (kr != kIOReturnSuccess)
        goto fail;

    kr = cmdCreateCq(MLX5E_DEFAULT_LOG_RQ_SIZE, &fRxCqn,
                     &fRxCqBuf, &fRxCqDesc, &fRxCqMap, &fRxCqDMA,
                     &fRxCqDbOffset);
    if (kr != kIOReturnSuccess)
        goto fail;
    kr = cmdCreateRq(fRxCqn, &fRqn, &fRxDbOffset);
    if (kr != kIOReturnSuccess)
        goto fail;
    kr = cmdCreateTir(fRqn, fTd, &fTirn);
    if (kr != kIOReturnSuccess)
        goto fail;
    /* Post the initial RX WQEs (See mlx5e_post_rx_wqes) */
    for (uint16_t i = 0; i < (1u << MLX5E_DEFAULT_LOG_RQ_SIZE); i++) {
        kr = postRxWqe(i);
        if (kr != kIOReturnSuccess)
            goto fail;
        fRxPc++;
    }
    mlxMemoryBarrier();
    *fRxRing->getDbRecord() = OSSwapHostToBigInt32(fRxPc);
    kr = cmdModifyRqReady(fRqn);
    if (kr != kIOReturnSuccess)
        goto fail;
    return kIOReturnSuccess;

fail:
    destroyRxResources();
    return kr;
}

kern_return_t MlxEthernet::destroyTxResources()
{
    kern_return_t kr;
    if (fSqn != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyObject(MLX_CMD_OP_DESTROY_SQ, fSqn);
        if (kr != kIOReturnSuccess)
            return kr;
        fSqn = MLX5E_INVALID_RESOURCE;
    }
    for (uint16_t i = 0; i < (1u << MLX5E_DEFAULT_LOG_SQ_SIZE); i++) {
        mbuf_t m = fTxRing ? fTxRing->getWqeMbuf(i) : NULL;
        if (m) {
            mbuf_freem(m);
            fTxRing->clearWqeMbuf(i);
        }
    }
    if (fTxBufDesc) {
        mlxUnmapDMA(fTxBufMap);
        fTxBufMap = NULL;
        fTxBufDesc->release();
        fTxBufDesc = NULL;
        fTxBufDMA = 0;
    }
    if (fTxDbOffset != MLX5E_INVALID_RESOURCE) {
        fCore->getUAR()->freeDbSlots(fTxDbOffset, 2);
        fTxDbOffset = MLX5E_INVALID_RESOURCE;
        if (fTxRing) {
            fTxRing->setDbRecord(NULL);
            fTxRing->setDbDMA(0);
        }
    }
    if (fTxBfValid) {
        fCore->getUAR()->freeBfreg(&fTxBf);
        memset(&fTxBf, 0, sizeof(fTxBf));
        fTxBfValid = false;
    }
    if (fTxCqn != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyCq(fTxCqn);
        if (kr != kIOReturnSuccess)
            return kr;
        fTxCqn = MLX5E_INVALID_RESOURCE;
    }
    if (fTxCqDesc) {
        mlxUnmapDMA(fTxCqMap);
        fTxCqMap = NULL;
        fTxCqDesc->release();
        fTxCqDesc = NULL;
        fTxCqBuf = NULL;
        fTxCqDMA = 0;
    }
    if (fTxCqDbOffset != MLX5E_INVALID_RESOURCE) {
        fCore->getUAR()->freeDbSlots(fTxCqDbOffset, 2);
        fTxCqDbOffset = MLX5E_INVALID_RESOURCE;
    }
    if (fTisn != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyObject(MLX_CMD_OP_DESTROY_TIS, fTisn);
        if (kr != kIOReturnSuccess)
            return kr;
        fTisn = MLX5E_INVALID_RESOURCE;
    }
    fTxPc = fTxCc = 0;
    fTxCqCc = fTxArmSn = 0;
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::destroyRxResources()
{
    kern_return_t kr;
    if (fTirn != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyObject(MLX_CMD_OP_DESTROY_TIR, fTirn);
        if (kr != kIOReturnSuccess)
            return kr;
        fTirn = MLX5E_INVALID_RESOURCE;
    }
    if (fRqn != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyObject(MLX_CMD_OP_DESTROY_RQ, fRqn);
        if (kr != kIOReturnSuccess)
            return kr;
        fRqn = MLX5E_INVALID_RESOURCE;
    }
    if (fRxBufDesc) {
        mlxUnmapDMA(fRxBufMap);
        fRxBufMap = NULL;
        fRxBufDesc->release();
        fRxBufDesc = NULL;
        fRxBufDMA = 0;
    }
    if (fRxDbOffset != MLX5E_INVALID_RESOURCE) {
        fCore->getUAR()->freeDbSlots(fRxDbOffset, 2);
        fRxDbOffset = MLX5E_INVALID_RESOURCE;
        if (fRxRing) {
            fRxRing->setDbRecord(NULL);
            fRxRing->setDbDMA(0);
        }
    }
    if (fRxCqn != MLX5E_INVALID_RESOURCE) {
        kr = cmdDestroyCq(fRxCqn);
        if (kr != kIOReturnSuccess)
            return kr;
        fRxCqn = MLX5E_INVALID_RESOURCE;
    }
    if (fRxCqDesc) {
        mlxUnmapDMA(fRxCqMap);
        fRxCqMap = NULL;
        fRxCqDesc->release();
        fRxCqDesc = NULL;
        fRxCqBuf = NULL;
        fRxCqDMA = 0;
    }
    if (fRxCqDbOffset != MLX5E_INVALID_RESOURCE) {
        fCore->getUAR()->freeDbSlots(fRxCqDbOffset, 2);
        fRxCqDbOffset = MLX5E_INVALID_RESOURCE;
    }
    fRxPc = fRxCc = 0;
    fRxCqCc = fRxArmSn = 0;
    return kIOReturnSuccess;
}

/* ========== mlx5e completion handling (See mlx5e_poll_tx_cq / mlx5e_poll_rx_cq) ========== */

void MlxEthernet::pollTxCq()
{
    /* TX CQ: free the mbufs of completed WQEs (See mlx5e_poll_tx_cq, en_tx.c) */
    if (!fTxRing || !fTxCqBuf)
        return;
    IOLockLock(fLock);
    MlxCqe64 *cqeBuf = (MlxCqe64 *)fTxCqBuf;
    uint32_t size = 1u << MLX5E_DEFAULT_LOG_SQ_SIZE;
    uint32_t ci = fTxCqCc & (size - 1);
    MlxCqe64 *cqe = &cqeBuf[ci];
    uint8_t sw_owner = (fTxCqCc >> MLX5E_DEFAULT_LOG_SQ_SIZE) & 1;
    while (MLX_CQE_GET_OPCODE(cqe) != MLX_CQE_INVALID &&
           (cqe->op_own & 1) == sw_owner) {
        mlxMemoryBarrier();
        uint16_t target = OSSwapBigToHostInt16(cqe->wqe_counter);
        do {
            uint16_t wqe_ix = fTxCc & (size - 1);
            mbuf_t m = fTxRing->getWqeMbuf(wqe_ix);
            if (m) {
                mbuf_freem(m);
                fTxRing->clearWqeMbuf(wqe_ix);
            }
            bool last = fTxCc == target;
            fTxCc++;
            if (last)
                break;
        } while (fTxCc != fTxPc);
        fTxCqCc++;
        ci = fTxCqCc & (size - 1);
        cqe = &cqeBuf[ci];
        sw_owner = (fTxCqCc >> MLX5E_DEFAULT_LOG_SQ_SIZE) & 1;
    }
    updateCqConsumer(fTxCqDbOffset, fTxCqCc);
    armCq(fTxCqn, fTxCqDbOffset, fTxArmSn, fTxCqCc);
    IOLockUnlock(fLock);
    IOOutputQueue *queue = getOutputQueue();
    if (queue)
        queue->service();
}

void MlxEthernet::pollRxCq()
{
    if (!fRxRing || !fRxCqBuf)
        return;
    MlxCqe64 *cqeBuf = (MlxCqe64 *)fRxCqBuf;
    uint32_t size = 1u << MLX5E_DEFAULT_LOG_RQ_SIZE;
    uint32_t ci = fRxCqCc & (size - 1);
    MlxCqe64 *cqe = &cqeBuf[ci];
    uint8_t sw_owner = (fRxCqCc >> MLX5E_DEFAULT_LOG_RQ_SIZE) & 1;
    while (MLX_CQE_GET_OPCODE(cqe) != MLX_CQE_INVALID &&
           (cqe->op_own & 1) == sw_owner) {
        mlxMemoryBarrier();
        handleRxCqe(cqe);
        fRxCqCc++;
        ci = fRxCqCc & (size - 1);
        cqe = &cqeBuf[ci];
        sw_owner = (fRxCqCc >> MLX5E_DEFAULT_LOG_RQ_SIZE) & 1;
    }
    updateCqConsumer(fRxCqDbOffset, fRxCqCc);
    armCq(fRxCqn, fRxCqDbOffset, fRxArmSn, fRxCqCc);
}

kern_return_t MlxEthernet::postRxWqe(uint16_t index)
{
    /* Post one RECV WQE pointing at the RX buffer pool slot (See mlx5e_post_rx_wqes)
     * Each cyclic RQ WQE occupies one 64-byte stride; the data segment sits at
     * the start and the rest of the stride must be zeroed. */
    if (!fRxRing || !fRxRing->getWqBuf())
        return kIOReturnNotReady;
    if (index >= MLX5E_MAX_RX_BUFS)
        return kIOReturnBadArgument;
    uint8_t *wqe = (uint8_t *)fRxRing->getWqBuf() + (index * 64);
    memset(wqe, 0, 64);
    struct MlxWqeDataSeg *dseg = (struct MlxWqeDataSeg *)wqe;
    dseg->byte_count = OSSwapHostToBigInt32(MLX5E_RX_BUF_SIZE);
    dseg->lkey = OSSwapHostToBigInt32(fLkey);
    dseg->addr = OSSwapHostToBigInt64(fRxBufDMA + (uint64_t)index * MLX5E_RX_BUF_SIZE);
    mlxMemoryBarrier();
    return kIOReturnSuccess;
}

void MlxEthernet::updateCqConsumer(uint32_t dbOffset, uint32_t consumer)
{
    if (dbOffset == MLX5E_INVALID_RESOURCE || !fCore || !fCore->getUAR())
        return;
    uint32_t *db = fCore->getUAR()->getDbRecord();
    if (db) {
        db[dbOffset / 4] =
            OSSwapHostToBigInt32(consumer & 0x00ffffff);
        mlxMemoryBarrier();
    }
}

void MlxEthernet::armCq(uint32_t cqn, uint32_t dbOffset, uint32_t armSn,
                        uint32_t consumer)
{
    if (cqn == MLX5E_INVALID_RESOURCE ||
        dbOffset == MLX5E_INVALID_RESOURCE || !fCore || !fCore->getUAR())
        return;
    MlxUAR *uar = fCore->getUAR();
    uint32_t *db = uar->getDbRecord();
    IOVirtualAddress uarAddress = uar->getUarVirtualAddress();
    if (!db || !uarAddress)
        return;
    uint32_t arm = ((armSn & 3) << 28) | (consumer & 0x00ffffff);
    db[dbOffset / 4 + 1] = OSSwapHostToBigInt32(arm);
    mlxMemoryBarrier();
    alignas(8) uint32_t doorbell[2] = {
        OSSwapHostToBigInt32(arm),
        OSSwapHostToBigInt32(cqn & 0x00ffffff)
    };
    volatile uint64_t *mmio = reinterpret_cast<volatile uint64_t *>(
        static_cast<uintptr_t>(uarAddress) + MLX_CQ_DOORBELL);
    *mmio = *reinterpret_cast<uint64_t *>(doorbell);
    OSSynchronizeIO();
}

void MlxEthernet::handleEvent(uint32_t type, void *eqe)
{
    if (type == MLX_EVENT_TYPE_PORT_STATE_CHANGE) {
        setLinkState(queryLinkState(), 0);
        return;
    }
    /* EQ completion event → poll the matching CQ (See mlx5e_completion_event) */
    if (type != MLX_EVENT_TYPE_COMPLETION || !eqe)
        return;
    MlxEqe *eq = (MlxEqe *)eqe;
    uint32_t cqn = OSSwapBigToHostInt32(eq->data.comp.cqn) & 0xFFFFFF;
    if (cqn == fTxCqn) {
        fTxArmSn++;
        pollTxCq();
    } else if (cqn == fRxCqn) {
        fRxArmSn++;
        pollRxCq();
    }
}

/* ========== MlxEthernetDriver ========== */

bool MlxEthernetDriver::init(OSDictionary *properties)
{
    if (!super2::init(properties))
        return false;
    fCore = NULL;
    fEth = NULL;
    return true;
}

bool MlxEthernetDriver::start(IOService *provider)
{
    if (!super2::start(provider))
        return false;

    fCore = OSDynamicCast(MlxPCIDriver, provider);
    if (!fCore) {
        IOLog("MlxEthernetDriver: provider is not the core layer\n");
        return false;
    }
    fCore->retain();

    /* Link layer check: only create the Ethernet interface for Ethernet ports (see main.c:1771)
     * IB ports use a separate IB data path (reserved for Option C) */
    if (fCore->getHCA() && fCore->getHCA()->caps().isIB()) {
        IOLog("MlxEthernetDriver: IB port, skipping Ethernet interface (reserved for Option C)\n");
        return true;
    }

    /* Create the Ethernet controller */
    fEth = OSTypeAlloc(MlxEthernet);
    if (!fEth || !fEth->init()) {
        IOLog("MlxEthernetDriver: controller init failed\n");
        goto fail;
    }
    if (!fEth->start(provider)) {
        fEth->release();
        fEth = NULL;
        goto fail;
    }
    fEth->attach(provider);
    fEth->registerService();
    return true;

fail:
    if (fEth) {
        fEth->release();
        fEth = NULL;
    }
    if (fCore) {
        fCore->release();
        fCore = NULL;
    }
    return false;
}

void MlxEthernetDriver::stop(IOService *provider)
{
    if (fEth) {
        fEth->stop(provider);
        fEth->release();
        fEth = NULL;
    }
    if (fCore) {
        fCore->release();
        fCore = NULL;
    }
    super2::stop(provider);
}

void MlxEthernetDriver::free()
{
    if (fEth) {
        fEth->release();
        fEth = NULL;
    }
    if (fCore) {
        fCore->release();
        fCore = NULL;
    }
    super2::free();
}
