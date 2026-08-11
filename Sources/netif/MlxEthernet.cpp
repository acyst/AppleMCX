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
    fWqDesc = NULL;
    fWqDmaMap = NULL;
    fDbRecord = NULL;
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
    }
}

void MlxEthRing::updateDb(uint16_t head)
{
    fHead = head;
    if (fDbRecord)
        *fDbRecord = head;
    mlxMemoryBarrier();
    /* The doorbell write (BF) is done in the xmit path */
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
    fLock = IOLockAlloc();
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

    /* Allocate the TX/RX ring buffers */
    fTxRing = OSTypeAlloc(MlxEthRing);
    if (!fTxRing || !fTxRing->init(256, 64)) {
        IOLog("MlxEthernet: TX ring allocation failed\n");
        return false;
    }
    fRxRing = OSTypeAlloc(MlxEthRing);
    if (!fRxRing || !fRxRing->init(256, 64)) {
        IOLog("MlxEthernet: RX ring allocation failed\n");
        return false;
    }

    /* Allocate a DMA-coherent WQ buffer (TX data path) */
    if (fTxRing->allocBuffers() != kIOReturnSuccess) {
        IOLog("MlxEthernet: TX buffer allocation failed\n");
        return false;
    }

    /* TX parameters (MVP: fixed QPN/lkey, full SQ creation later in P2) */
    fTxQpn = 1;
    fTxLkey = 1;

    /* Read the MAC address (see mlx5_query_nic_vport_mac_address) */
    uint8_t mac[6] = {0x00, 0x02, 0xC9, 0x00, 0x00, 0x01};
    memcpy(fMacAddr, mac, 6);

    /* Create and attach the Ethernet interface to the kernel protocol stack */
    if (!attachInterface(&fNic, true)) {
        IOLog("MlxEthernet: interface creation failed\n");
        return false;
    }
    fNetif = OSDynamicCast(IOEthernetInterface, fNic);

    IOLog("MlxEthernet: interface ready MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

void MlxEthernet::stop(IOService *provider)
{
    releaseResources();
    super::stop(provider);
}

void MlxEthernet::releaseResources()
{
    if (fNic) {
        detachInterface(fNic, true);
        fNic->release();
        fNic = NULL;
        fNetif = NULL;
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
    fEnabled = false;
    setLinkState(false, 0);
    return kIOReturnUnsupported;
}

IOReturn MlxEthernet::disable(IONetworkInterface *netif)
{
    fEnabled = false;
    setLinkState(false, 0);
    return kIOReturnSuccess;
}

IOReturn MlxEthernet::setPromiscuousMode(bool active)
{
    return kIOReturnSuccess;
}

IOReturn MlxEthernet::setMulticastMode(bool active)
{
    return kIOReturnSuccess;
}

IOReturn MlxEthernet::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    if (group == gIONetworkFilterGroup) {
        *filters = kIOPacketFilterUnicast | kIOPacketFilterMulticast |
                   kIOPacketFilterBroadcast | kIOPacketFilterPromiscuous;
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
    if (!fEnabled)
        return kIOReturnOutputStall;

    /* MVP simplification: call xmitInline (build a WQE, write it to the SQ)
     * Full path: build ctrl+eth+data segments → write the SQ → ring the doorbell */
    kern_return_t kr = xmitPacket(packet);
    if (kr != kIOReturnSuccess) {
        return kIOReturnOutputStall;
    }
    return kIOReturnOutputSuccess;
}

kern_return_t MlxEthernet::xmitPacket(mbuf_t packet)
{
    /* No safe ownership transfer exists until SQ/TIS/CQ completion support is
     * implemented. Keep the unfinished Ethernet data path disabled. */
    (void)packet;
    return kIOReturnUnsupported;
#if 0
    /* See mlx5e_xmit (en_tx.c:666) + mlx5e_sq_xmit_wqe:
     * WQE = ctrl(16B) + eth(16B) + data seg (DMA)
     * Write to the SQ ring buffer → update the DB record → ring the doorbell */
    if (!packet || !fTxRing || !fTxRing->getWqBuf())
        return kIOReturnInvalid;

    uint32_t len = mbuf_pkthdr_len(packet);
    if (len == 0) {
        mbuf_freem(packet);
        return kIOReturnSuccess;
    }

    /* Get an SQ slot */
    uint16_t head = fTxRing->getHead();
    uint16_t idx = head & (fTxRing->getSize() - 1);

    /* WQE: ctrl + eth + data seg (see mlx5e_tx_wqe, en.h:244) */
    MlxEthTxWqe *wqe = (MlxEthTxWqe *)fTxRing->getWqebb(idx);
    memset(wqe, 0, 48);   /* ctrl16 + eth16 + data16 */

    /* ctrl segment (see mlx5r_finish_wqe, wr.c:758) */
    wqe->ctrl.opmod_idx_opcode = OSSwapHostToBigInt32(
        (MLX_OPCODE_SEND << 0) | (idx << 8));
    wqe->ctrl.qpn_ds = OSSwapHostToBigInt32(
        (3u << 16) | (fTxQpn & 0xFFFF));   /* 3 segments, QPN */
    wqe->ctrl.fm_ce_se = 0x02;             /* ce: generate a completion */

    /* eth segment (see mlx5e_txwqe_build_eseg, en_tx.c:656)
     * MVP: no checksum offload (cs_flags=0), hardware inlines the Ethernet header */
    wqe->eth.cs_flags = 0;

    /* data segment: mbuf data → DMA address (see mlx5e_sq_xmit_wqe) */
    /* MVP: translate the mbuf data address to a physical address via the DMA service */
    uint8_t *data = (uint8_t *)mbuf_data(packet);
    uint64_t phys = 0;
    if (fCore && fCore->getDMA()) {
        MlxDMAReq req = {};
        if (fCore->getDMA()->pinUserMemory((uint64_t)(uintptr_t)data,
                                           len, &req) == kIOReturnSuccess) {
            phys = req.paList[0];
            fCore->getDMA()->unpinMemory(&req);
        }
    }
    wqe->data[0].byte_count = OSSwapHostToBigInt32(len);
    wqe->data[0].lkey = OSSwapHostToBigInt32(fTxLkey);
    wqe->data[0].addr = OSSwapHostToBigInt64(phys ? phys
                                                  : (uint64_t)(uintptr_t)data);

    mlxMemoryBarrier();

    /* Update the DB record + doorbell (see the doorbell write in mlx5e_sq_xmit_wqe) */
    fTxRing->setHead(head + 1);
    fTxRing->updateDb(head + 1);

    /* Free the mbuf (data is referenced by DMA; MVP frees it immediately) */
    mbuf_freem(packet);
    return kIOReturnSuccess;
#endif
}

kern_return_t MlxEthernet::xmitInline(mbuf_t packet)
{
    return kIOReturnUnsupported;
}

kern_return_t MlxEthernet::xmitDma(mbuf_t packet)
{
    return kIOReturnUnsupported;
}

/* RX: see mlx5e_handle_rx_cqe (en_rx.c:70) */
void MlxEthernet::handleRxCqe(struct MlxCqe64 *cqe)
{
    (void)cqe;
}

kern_return_t MlxEthernet::receivePacket(mbuf_t packet, UInt32 length)
{
    /* After receiving a packet from the hardware RX ring, hand it to the kernel protocol stack */
    if (!fEnabled) {
        mbuf_freem(packet);
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
        return false;
    }
    if (!fEth->start(provider)) {
        fEth->release();
        fEth = NULL;
        return false;
    }
    fEth->attach(provider);
    fEth->registerService();
    return true;
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
    super2::free();
}
