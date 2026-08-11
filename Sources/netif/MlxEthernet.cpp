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
    fLock = IOLockAlloc();

    /* mlx5e resources */
    fPd = 0;
    fTd = 0;
    fTisn = 0;
    fSqn = 0;
    fRqn = 0;
    fTirn = 0;
    fTxCqn = 0;
    fRxCqn = 0;
    fTxDbOffset = 0;
    fRxDbOffset = 0;
    fTxCqDbOffset = 0;
    fRxCqDbOffset = 0;
    fTxBf = NULL;
    fTxCqDesc = NULL;
    fTxCqDMA = 0;
    fRxCqDesc = NULL;
    fRxCqDMA = 0;
    fRxBufDesc = NULL;
    fRxBufMap = NULL;
    fRxBufDMA = 0;
    fTxPc = 0;
    fTxCc = 0;
    fRxPc = 0;
    fRxCc = 0;
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

    /* Read the MAC address (see mlx5_query_nic_vport_mac_address) */
    uint8_t mac[6] = {0x00, 0x02, 0xC9, 0x00, 0x00, 0x01};
    memcpy(fMacAddr, mac, 6);

    /* Subscribe to EQ completion events (See mlx5e_completion_event, txrx.h)
     * The EQ dispatches completion EQEs to this notifier (by cqn). */
    MlxEQ *eq = fCore->getEQ();
    if (eq) {
        eq->registerNotifier(MLX_EVENT_TYPE_COMPLETION, this);
        fNotifierRegistered = true;
    }

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
    /* Stop the data path first (destroy the mlx5e queues) */
    if (fEnabled)
        disable(fNic);
    releaseResources();
    super::stop(provider);
}

void MlxEthernet::releaseResources()
{
    if (fCore && fCore->getEQ() && fNotifierRegistered) {
        fCore->getEQ()->unregisterNotifier(MLX_EVENT_TYPE_COMPLETION, this);
        fNotifierRegistered = false;
    }
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
    IOReturn kr;

    /* Create the mlx5e TX/RX resources (See mlx5e_open_locked, en_main.c) */
    kr = allocPdTd();
    if (kr != kIOReturnSuccess) {
        IOLog("MlxEthernet: PD/TD allocation failed\n");
        return kr;
    }
    kr = createTxResources();
    if (kr != kIOReturnSuccess) {
        IOLog("MlxEthernet: TX resource creation failed\n");
        goto fail;
    }
    kr = createRxResources();
    if (kr != kIOReturnSuccess) {
        IOLog("MlxEthernet: RX resource creation failed\n");
        goto fail_tx;
    }

    /* Enable the data path (See mlx5e_activate_priv_channels) */
    fEnabled = true;
    setLinkState(true, 10000);
    return kIOReturnSuccess;

fail_tx:
    destroyTxResources();
fail:
    deallocPdTd();
    fEnabled = false;
    setLinkState(false, 0);
    return kr;
}

IOReturn MlxEthernet::disable(IONetworkInterface *netif)
{
    (void)netif;
    fEnabled = false;
    setLinkState(false, 0);
    destroyRxResources();
    destroyTxResources();
    deallocPdTd();
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
    (void)param;
    if (!fEnabled || !packet || !fTxRing || !fSqn)
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
    if (!packet || !fTxRing || !fTxRing->getWqBuf() || !fSqn)
        return kIOReturnInvalid;

    uint32_t len = mbuf_pkthdr_len(packet);
    if (len == 0) {
        mbuf_freem(packet);
        return kIOReturnSuccess;
    }
    if (len > 0xFFFF) {
        mbuf_freem(packet);
        return kIOReturnInvalid;
    }

    IOLockLock(fLock);
    /* Check ring room (See mlx5e_wqc_has_room_for, txrx.h) */
    if ((uint16_t)(fTxPc - fTxCc) >= fTxRing->getSize()) {
        IOLockUnlock(fLock);
        return kIOReturnOutputStall;
    }

    /* Get an SQ slot */
    uint16_t pi = fTxPc & (fTxRing->getSize() - 1);

    /* WQE: ctrl + eth + data seg (see mlx5e_tx_wqe, en.h:244) */
    MlxEthTxWqe *wqe = (MlxEthTxWqe *)fTxRing->getWqebb(pi);
    memset(wqe, 0, 48);   /* ctrl16 + eth16 + data16 */

    /* ctrl segment (see mlx5e_txwqe_complete, en_tx.c)
     *   opmod_idx_opcode = (pc << 8) | opcode
     *   qpn_ds           = (sqn << 8) | ds_cnt   (ds in 16-byte units) */
    wqe->ctrl.opmod_idx_opcode = OSSwapHostToBigInt32(
        ((uint32_t)fTxPc << 8) | MLX_OPCODE_SEND);
    wqe->ctrl.qpn_ds = OSSwapHostToBigInt32(
        (fSqn << 8) | 3u);          /* 3 DS: ctrl+eth+data */

    /* eth segment (see mlx5e_txwqe_build_eseg_csum, en_tx.c)
     * MVP: no checksum offload (cs_flags=0), hardware inserts the header */
    wqe->eth.cs_flags = 0;

    /* data segment: mbuf data → DMA address (see mlx5e_txwqe_build_dsegs) */
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
    wqe->data[0].lkey = OSSwapHostToBigInt32(0x1);
    wqe->data[0].addr = OSSwapHostToBigInt64(phys ? phys
                                                  : (uint64_t)(uintptr_t)data);

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
    if (fTxBf) {
        /* mlx5_write64(ctrl, uar_map): write the first 8 bytes of the ctrl
         * segment as the blue-flame doorbell */
        volatile uint64_t *bf = (volatile uint64_t *)fTxBf;
        *bf = *(volatile uint64_t *)&w->ctrl;
    }
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
    uint32_t bcnt = OSSwapBigToHostInt32(cqe->byte_cnt);
    if (bcnt == 0 || bcnt > MLX5E_RX_BUF_SIZE)
        return;

    /* The packet data was DMA'd into the RX buffer pool slot. Allocate an
     * mbuf, copy the payload, and hand it to the kernel protocol stack. */
    uint16_t wqe_idx = fRxCc & ((1u << MLX5E_DEFAULT_LOG_RQ_SIZE) - 1);
    uint8_t *src = (uint8_t *)fRxBufDesc->getBytesNoCopy() +
                   ((uint32_t)wqe_idx * MLX5E_RX_BUF_SIZE);

    mbuf_t m = NULL;
    if (mbuf_allocpacket(MBUF_DONTWAIT, bcnt, NULL, &m) == 0 && m) {
        memcpy(mbuf_data(m), src, bcnt);
        mbuf_setlen(m, bcnt);
        mbuf_pkthdr_setlen(m, bcnt);
    }
    /* Repost the RX WQE slot */
    postRxWqe(wqe_idx);
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

kern_return_t MlxEthernet::cmdCreateTis(uint32_t td, uint32_t pd, uint32_t *tisn)
{
    /* CREATE_TIS (0x912). tisc at bit 0x100:
     *   transport_domain at +0x128 (24b), pd at +0x168 (24b) */
    uint8_t in[0x80] = {};
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
                                       uint64_t *cqDMA,
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
    mlxSetBits(in, 0x2e0, 1, 1);           /* cq_umem_valid */
    OSWriteBigInt64(in, 0x880 / 8, dma);

    uint32_t inSize = 0x880 / 8 + 8;
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
    if (cqDMA)   *cqDMA = dma;
    if (dbOffset) *dbOffset = dbOff;
    mlxUnmapDMA(map);
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::cmdDestroyCq(uint32_t cqn)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_CQ);
    OSWriteBigInt32(in, 4, cqn);
    return fCore->exec(MLX_CMD_OP_DESTROY_CQ, in, sizeof(in),
                       out, sizeof(out), 5000);
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
    mlxSetBits(wq, 0x120, 1, 1);           /* dbr_umem_valid */
    mlxSetBits(wq, 0x121, 1, 1);           /* wq_umem_valid */
    /* PAS at wq bit 0x600 = input byte 0x50 + 0xc0 = 0x110 */
    OSWriteBigInt64(in, 0x110, fTxRing->getWqDMA());

    uint32_t inSize = 0x110 + 8;
    kern_return_t kr = fCore->exec(MLX_CMD_OP_CREATE_SQ, in, inSize,
                                   out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        fCore->getUAR()->freeDbSlots(dbOff, 2);
        return kr;
    }

    if (sqn)     *sqn = mlx5e_out_obj(out);
    if (dbOffset) *dbOffset = dbOff;

    /* Point the ring doorbell record at the SQ DB slot */
    fTxRing->setDbRecord(fCore->getUAR()->getDbRecord() + dbOff / 4);
    fTxRing->setDbDMA(fCore->getUAR()->getDbRecordDMA() + dbOff);
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::cmdCreateRq(uint32_t cqn, uint32_t *rqn,
                                       uint32_t *dbOffset)
{
    /* CREATE_RQ (0x908). rqc at bit 0x100:
     *   mem_rq_type at +0x4 (4b, INLINE=0), state at +0x8 (4b, RDY=1),
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
    mlxSetBits(rqc, 0x08, 4, 1);           /* state RDY */
    mlxSetBits(rqc, 0x48, 24, cqn);
    uint8_t *wq = rqc + 0x30;              /* wq at byte 0x50 */
    mlxSetBits(wq, 0x00, 4, 1);            /* wq_type CYCLIC */
    mlxSetBits(wq, 0x48, 24, fPd);
    mlxSetBits(wq, 0x68, 24, fCore->getUAR()->getBootUarIndex());
    mlxSetBits(wq, 0x80, 64, fCore->getUAR()->getDbRecordDMA() + dbOff);
    mlxSetBits(wq, 0x10c, 4, 6);           /* log_wq_stride = 64B */
    mlxSetBits(wq, 0x113, 5, 0);           /* 4K pages */
    mlxSetBits(wq, 0x11b, 5, logSize);
    mlxSetBits(wq, 0x120, 1, 1);           /* dbr_umem_valid */
    mlxSetBits(wq, 0x121, 1, 1);           /* wq_umem_valid */
    OSWriteBigInt64(in, 0x110, fRxRing->getWqDMA());

    uint32_t inSize = 0x110 + 8;
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
    uint8_t in[0x80] = {};
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

kern_return_t MlxEthernet::allocPdTd()
{
    kern_return_t kr = cmdAllocPd(&fPd);
    if (kr != kIOReturnSuccess)
        return kr;
    return cmdAllocTd(&fTd);
}

void MlxEthernet::deallocPdTd()
{
    if (fTd) {
        uint8_t in[16] = {};
        uint8_t out[16] = {};
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DEALLOC_TRANSPORT_DOMAIN);
        OSWriteBigInt32(in, 4, fTd);
        fCore->exec(MLX_CMD_OP_DEALLOC_TRANSPORT_DOMAIN, in, sizeof(in),
                    out, sizeof(out), 5000);
        fTd = 0;
    }
    if (fPd) {
        uint8_t in[16] = {};
        uint8_t out[16] = {};
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DEALLOC_PD);
        OSWriteBigInt32(in, 4, fPd);
        fCore->exec(MLX_CMD_OP_DEALLOC_PD, in, sizeof(in),
                    out, sizeof(out), 5000);
        fPd = 0;
    }
}

kern_return_t MlxEthernet::createTxResources()
{
    /* TX: TIS + SQ + TX CQ (See mlx5e_create_tis / mlx5e_open_txqsq) */
    kern_return_t kr = cmdCreateTis(fTd, fPd, &fTisn);
    if (kr != kIOReturnSuccess)
        return kr;
    kr = cmdCreateCq(MLX5E_DEFAULT_LOG_SQ_SIZE, &fTxCqn, &fTxCqBuf,
                     &fTxCqDesc, &fTxCqDMA, &fTxCqDbOffset);
    if (kr != kIOReturnSuccess)
        return kr;
    kr = cmdCreateSq(fTisn, fTxCqn, &fSqn, &fTxDbOffset);
    if (kr != kIOReturnSuccess)
        return kr;
    /* Allocate a blue-flame doorbell register (See mlx5_alloc_bfreg) */
    MlxBfreg bf;
    if (fCore->getUAR()->allocBfreg(&bf) == kIOReturnSuccess)
        fTxBf = bf.map;
    else
        fTxBf = NULL;
    return kIOReturnSuccess;
}

kern_return_t MlxEthernet::createRxResources()
{
    /* RX: RQ + TIR + RX CQ (See mlx5e_open_rq / mlx5e_create_tir) */
    kern_return_t kr = cmdCreateCq(MLX5E_DEFAULT_LOG_RQ_SIZE, &fRxCqn,
                                   &fRxCqBuf, &fRxCqDesc, &fRxCqDMA,
                                   &fRxCqDbOffset);
    if (kr != kIOReturnSuccess)
        return kr;
    kr = cmdCreateRq(fRxCqn, &fRqn, &fRxDbOffset);
    if (kr != kIOReturnSuccess)
        return kr;
    kr = cmdCreateTir(fRqn, fTd, &fTirn);
    if (kr != kIOReturnSuccess)
        return kr;
    /* Allocate the RX buffer pool (DMA-coherent) and post initial WQEs */
    uint32_t poolBytes = MLX5E_MAX_RX_BUFS * MLX5E_RX_BUF_SIZE;
    fRxBufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, poolBytes, 0xFFFFFFF000ULL);
    if (!fRxBufDesc)
        return kIOReturnNoMemory;
    memset(fRxBufDesc->getBytesNoCopy(), 0, poolBytes);
    IODMACommand *rxMap = NULL;
    if (mlxMapDMAContiguous(fRxBufDesc, &rxMap, &fRxBufDMA) != kIOReturnSuccess) {
        fRxBufDesc->release();
        fRxBufDesc = NULL;
        return kIOReturnNoMemory;
    }
    fRxBufMap = rxMap;
    /* Post the initial RX WQEs (See mlx5e_post_rx_wqes) */
    for (uint16_t i = 0; i < (1u << MLX5E_DEFAULT_LOG_RQ_SIZE); i++)
        postRxWqe(i);
    return kIOReturnSuccess;
}

void MlxEthernet::destroyTxResources()
{
    if (fSqn) {
        uint8_t in[16] = {};
        uint8_t out[16] = {};
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_SQ);
        OSWriteBigInt32(in, 4, fSqn);
        fCore->exec(MLX_CMD_OP_DESTROY_SQ, in, sizeof(in), out, sizeof(out), 5000);
        fSqn = 0;
    }
    if (fTxCqn) {
        cmdDestroyCq(fTxCqn);
        fTxCqn = 0;
    }
    if (fTxCqDesc) {
        fTxCqDesc->release();
        fTxCqDesc = NULL;
        fTxCqBuf = NULL;
        fTxCqDMA = 0;
    }
    if (fTisn) {
        uint8_t in[16] = {};
        uint8_t out[16] = {};
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_TIS);
        OSWriteBigInt32(in, 4, fTisn);
        fCore->exec(MLX_CMD_OP_DESTROY_TIS, in, sizeof(in), out, sizeof(out), 5000);
        fTisn = 0;
    }
    if (fTxDbOffset) {
        fCore->getUAR()->freeDbSlots(fTxDbOffset, 2);
        fTxDbOffset = 0;
    }
    if (fTxBf) {
        fCore->getUAR()->freeBfreg(NULL);
        fTxBf = NULL;
    }
}

void MlxEthernet::destroyRxResources()
{
    if (fTirn) {
        uint8_t in[16] = {};
        uint8_t out[16] = {};
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_TIR);
        OSWriteBigInt32(in, 4, fTirn);
        fCore->exec(MLX_CMD_OP_DESTROY_TIR, in, sizeof(in), out, sizeof(out), 5000);
        fTirn = 0;
    }
    if (fRqn) {
        uint8_t in[16] = {};
        uint8_t out[16] = {};
        OSWriteBigInt16(in, 0, MLX_CMD_OP_DESTROY_RQ);
        OSWriteBigInt32(in, 4, fRqn);
        fCore->exec(MLX_CMD_OP_DESTROY_RQ, in, sizeof(in), out, sizeof(out), 5000);
        fRqn = 0;
    }
    if (fRxCqn) {
        cmdDestroyCq(fRxCqn);
        fRxCqn = 0;
    }
    if (fRxCqDesc) {
        fRxCqDesc->release();
        fRxCqDesc = NULL;
        fRxCqBuf = NULL;
        fRxCqDMA = 0;
    }
    if (fRxDbOffset) {
        fCore->getUAR()->freeDbSlots(fRxDbOffset, 2);
        fRxDbOffset = 0;
    }
    if (fRxBufDesc) {
        mlxUnmapDMA(fRxBufMap);
        fRxBufMap = NULL;
        fRxBufDesc->release();
        fRxBufDesc = NULL;
        fRxBufDMA = 0;
    }
}

/* ========== mlx5e completion handling (See mlx5e_poll_tx_cq / mlx5e_poll_rx_cq) ========== */

void MlxEthernet::pollTxCq()
{
    /* TX CQ: free the mbufs of completed WQEs (See mlx5e_poll_tx_cq, en_tx.c) */
    if (!fTxRing || !fTxCqBuf)
        return;
    MlxCqe64 *cqeBuf = (MlxCqe64 *)fTxCqBuf;
    uint32_t size = 1u << MLX5E_DEFAULT_LOG_SQ_SIZE;
    uint32_t ci = fTxCc & (size - 1);
    MlxCqe64 *cqe = &cqeBuf[ci];
    uint8_t sw_owner = (fTxCc >> MLX5E_DEFAULT_LOG_SQ_SIZE) & 1;
    while ((cqe->op_own & 1) == sw_owner) {
        mlxMemoryBarrier();
        /* Free the mbuf associated with this WQE (See mlx5e_tx_wqe_info.skb) */
        uint16_t wqe_ix = (uint16_t)(OSSwapBigToHostInt32(cqe->wqe_counter) & 0xFFFF) & (size - 1);
        mbuf_t m = fTxRing->getWqeMbuf(wqe_ix);
        if (m) {
            mbuf_freem(m);
            fTxRing->clearWqeMbuf(wqe_ix);
        }
        fTxCc++;
        ci = fTxCc & (size - 1);
        cqe = &cqeBuf[ci];
        sw_owner = (fTxCc >> MLX5E_DEFAULT_LOG_SQ_SIZE) & 1;
    }
    /* Update the CQ doorbell record (See mlx5_cqwq_update_db_record) */
    uint32_t *db = fCore->getUAR()->getDbRecord();
    if (db)
        db[fTxCqDbOffset / 4] = OSSwapHostToBigInt32(fTxCc & 0xFFFFFF);
}

void MlxEthernet::pollRxCq()
{
    if (!fRxRing || !fRxCqBuf)
        return;
    MlxCqe64 *cqeBuf = (MlxCqe64 *)fRxCqBuf;
    uint32_t size = 1u << MLX5E_DEFAULT_LOG_RQ_SIZE;
    uint32_t ci = fRxCc & (size - 1);
    MlxCqe64 *cqe = &cqeBuf[ci];
    uint8_t sw_owner = (fRxCc >> MLX5E_DEFAULT_LOG_RQ_SIZE) & 1;
    while ((cqe->op_own & 1) == sw_owner) {
        mlxMemoryBarrier();
        handleRxCqe(cqe);
        fRxCc++;
        ci = fRxCc & (size - 1);
        cqe = &cqeBuf[ci];
        sw_owner = (fRxCc >> MLX5E_DEFAULT_LOG_RQ_SIZE) & 1;
    }
    uint32_t *db = fCore->getUAR()->getDbRecord();
    if (db)
        db[fRxCqDbOffset / 4] = OSSwapHostToBigInt32(fRxCc & 0xFFFFFF);
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
    dseg->lkey = OSSwapHostToBigInt32(0x1);
    dseg->addr = OSSwapHostToBigInt64(fRxBufDMA + (uint64_t)index * MLX5E_RX_BUF_SIZE);
    mlxMemoryBarrier();
    return kIOReturnSuccess;
}

void MlxEthernet::handleEvent(uint32_t type, void *eqe)
{
    /* EQ completion event → poll the matching CQ (See mlx5e_completion_event) */
    if (type != MLX_EVENT_TYPE_COMPLETION || !eqe)
        return;
    MlxEqe *eq = (MlxEqe *)eqe;
    uint32_t cqn = OSSwapBigToHostInt32(eq->data.comp.cqn) & 0xFFFFFF;
    if (cqn == fTxCqn)
        pollTxCq();
    else if (cqn == fRxCqn)
        pollRxCq();
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
