/*
 * MlxEthernet.hpp — Ethernet interface layer (generic Mellanox family)
 *
 * On the macOS side, uses IOEthernetController to attach to the kernel protocol stack.
 * Corresponds to Linux mlx5e (drivers/net/ethernet/mellanox/mlx5/core/en_*.c)
 *
 * Note: RoCE v2 packets are bypassed by hardware (they do not go through the kernel protocol stack);
 *       this interface only handles regular TCP/IP traffic, coexisting with the RDMA data path.
 */
#ifndef MLX_ETHERNET_HPP
#define MLX_ETHERNET_HPP

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <sys/mbuf.h>
#include "MlxWQE.hpp"
#include "MlxUCIO.h"

class MlxRoCE;
class MlxPCIDriver;
class MlxEthRing;

/*
 * Ethernet controller — attaches to the kernel protocol stack
 */
class MlxEthernet : public IOEthernetController {
    OSDeclareDefaultStructors(MlxEthernet)

public:
    /* IOService lifecycle */
    virtual bool init(OSDictionary *properties = NULL) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

    /* IOEthernetController */
    virtual IOReturn enable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPromiscuousMode(bool active) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setMulticastMode(bool active) APPLE_KEXT_OVERRIDE;
    virtual UInt32 outputPacket(mbuf_t packet, void *param) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getPacketFilters(const OSSymbol *group,
                                      UInt32 *filters) const APPLE_KEXT_OVERRIDE;
    virtual IOReturn getMaxPacketSize(UInt32 *maxSize) const APPLE_KEXT_OVERRIDE;
    virtual IOReturn getMinPacketSize(UInt32 *minSize) const APPLE_KEXT_OVERRIDE;
    virtual IOReturn getHardwareAddress(IOEthernetAddress *addrP) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;

    /* RX path: called by the EQ completion interrupt after the hardware receives a packet */
    kern_return_t receivePacket(mbuf_t packet, UInt32 length);

    /* Update the link state */
    void setLinkState(bool up, UInt32 speedMbps);

    /* Access the core */
    MlxPCIDriver *getCore() { return fCore; }

private:
    /* TX: build a WQE and write it to the SQ (see mlx5e_xmit, en_tx.c:666) */
    kern_return_t xmitPacket(mbuf_t packet);
    kern_return_t xmitInline(mbuf_t packet);
    kern_return_t xmitDma(mbuf_t packet);

    /* RX: see mlx5e_handle_rx_cqe (en_rx.c:70) */
    void handleRxCqe(struct MlxCqe64 *cqe);

    MlxPCIDriver    *fCore;
    MlxRoCE         *fRoce;
    IOEthernetInterface *fNetif;
    IONetworkInterface *fNic;
    MlxEthRing      *fTxRing;
    MlxEthRing      *fRxRing;
    bool             fEnabled;
    bool             fLinkUp;
    UInt32           fLinkSpeed;      /* Mbps */
    uint8_t          fMacAddr[6];
    uint32_t         fTxQpn;          /* QPN of the Ethernet SQ */
    uint32_t         fTxLkey;         /* TX memory key */
    IOLock          *fLock;
};

/*
 * Ethernet driver binding layer — matches the nub published by the core layer (mlx_eth)
 * Responsibility: get the device handle from the core layer, create an MlxEthernet instance
 */
class MlxEthernetDriver : public IOService {
    OSDeclareDefaultStructors(MlxEthernetDriver)

public:
    virtual bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

private:
    MlxPCIDriver *fCore;
    MlxEthernet  *fEth;
};

/*
 * Ethernet ring buffer (TX/RX) — see mlx5e_txqsq / mlx5e_rq
 */
class MlxEthRing : public OSObject {
    OSDeclareDefaultStructors(MlxEthRing)

public:
    bool init(uint32_t size, uint32_t wqebbSize);
    void free();

    /* Allocate the WQ buffer + DB record (DMA-coherent) */
    kern_return_t allocBuffers();
    void freeBuffers();

    /* ring index */
    uint16_t nextIndex(uint16_t idx) const
    { return (idx + 1) & (fSize - 1); }

    uint16_t getSize() const { return fSize; }

    /* WQ buffer access */
    void *getWqebb(uint16_t idx) const
    { return (uint8_t *)fWqBuf + ((uint32_t)idx * fWqebbSize); }

    /* Doorbell update (see mlx5_wq_update_db) */
    void updateDb(uint16_t head);

    uint64_t getWqDMA() const { return fWqDMA; }
    void *getWqBuf() const { return fWqBuf; }
    uint16_t getHead() const { return fHead; }
    void setHead(uint16_t h) { fHead = h; }

private:
    uint16_t    fSize;
    uint16_t    fWqebbSize;
    uint16_t    fHead;
    uint16_t    fTail;
    void       *fWqBuf;
    uint64_t    fWqDMA;
    IOBufferMemoryDescriptor *fWqDesc;   /* WQ buffer descriptor */
    IODMACommand *fWqDmaMap;
    uint32_t   *fDbRecord;    /* doorbell record */
    uint64_t    fDbDMA;
};

#endif /* MLX_ETHERNET_HPP */
