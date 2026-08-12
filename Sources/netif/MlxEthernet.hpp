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
#include "MlxEQ.hpp"   /* MlxEventNotifier: Ethernet subscribes to completion events */
#include "MlxUAR.hpp"

/* mlx5e data-path sizing (See en.h MLX5E_PARAMS_*) */
#define MLX5E_DEFAULT_LOG_SQ_SIZE  8      /* 256 WQEs */
#define MLX5E_DEFAULT_LOG_RQ_SIZE  8      /* 256 WQEs */
#define MLX5E_RX_BUF_SIZE         2048
#define MLX5E_TX_BUF_SIZE         2048
#define MLX5E_MAX_RX_BUFS          256
#define MLX5E_INVALID_RESOURCE 0xffffffffu

class MlxRoCE;
class MlxPCIDriver;
class MlxEthRing;

/*
 * Ethernet controller — attaches to the kernel protocol stack
 */
class MlxEthernet : public IOEthernetController, public MlxEventNotifier {
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

    /* EQ completion event: dispatch to TX/RX CQ polling (See mlx5e_completion_event) */
    virtual void handleEvent(uint32_t type, void *eqe) APPLE_KEXT_OVERRIDE;
    virtual OSObject *notifierObject() APPLE_KEXT_OVERRIDE { return this; }

    /* RX path: called by the EQ completion interrupt after the hardware receives a packet */
    kern_return_t receivePacket(mbuf_t packet, UInt32 length);

    /* Update the link state */
    void setLinkState(bool up, UInt32 speedMbps);

    /* Access the core */
    MlxPCIDriver *getCore() { return fCore; }

private:
    void releaseResources();

    /* mlx5e resource creation/teardown (See mlx5e_open_locked, en_main.c) */
    kern_return_t allocGlobalResources();
    kern_return_t deallocGlobalResources();
    kern_return_t createTxResources();
    kern_return_t createRxResources();
    kern_return_t destroyTxResources();
    kern_return_t destroyRxResources();

    /* Low-level commands (See en_main.c / transobj.c) */
    kern_return_t cmdAllocTd(uint32_t *td);
    kern_return_t cmdAllocPd(uint32_t *pd);
    kern_return_t cmdCreateMkey(uint32_t pd, uint32_t *mkeyIndex,
                                uint32_t *lkey);
    kern_return_t cmdDestroyObject(uint32_t opcode, uint32_t objectId);
    kern_return_t cmdCreateTis(uint32_t td, uint32_t pd, uint32_t *tisn);
    kern_return_t cmdCreateCq(uint32_t logSize, uint32_t *cqn, void **cqBuf,
                              IOBufferMemoryDescriptor **cqDesc,
                              IODMACommand **cqMap, uint64_t *cqDMA,
                              uint32_t *dbOffset);
    kern_return_t cmdDestroyCq(uint32_t cqn);
    kern_return_t cmdCreateSq(uint32_t tisn, uint32_t cqn, uint32_t *sqn,
                              uint32_t *dbOffset);
    kern_return_t cmdCreateRq(uint32_t cqn, uint32_t *rqn, uint32_t *dbOffset);
    kern_return_t cmdCreateTir(uint32_t rqn, uint32_t td, uint32_t *tirn);
    kern_return_t cmdModifySqReady(uint32_t sqn);
    kern_return_t cmdModifyRqReady(uint32_t rqn);
    kern_return_t createRxFlowSteering();
    kern_return_t destroyRxFlowSteering();
    kern_return_t queryMacAddress(uint8_t mac[6]);
    bool queryLinkState();

    /* TX: build a WQE and write it to the SQ (see mlx5e_xmit, en_tx.c:666) */
    kern_return_t xmitPacket(mbuf_t packet);
    kern_return_t xmitInline(mbuf_t packet);
    kern_return_t xmitDma(mbuf_t packet);

    /* RX: see mlx5e_handle_rx_cqe (en_rx.c:70) */
    void handleRxCqe(struct MlxCqe64 *cqe);

    /* Completion polling (See mlx5e_poll_tx_cq / mlx5e_poll_rx_cq) */
    void pollTxCq();
    void pollRxCq();
    kern_return_t postRxWqe(uint16_t index);
    void updateCqConsumer(uint32_t dbOffset, uint32_t consumer);
    void armCq(uint32_t cqn, uint32_t dbOffset, uint32_t armSn,
               uint32_t consumer);

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
    uint8_t          fTxMinInlineMode;
    IOLock          *fLock;

    /* mlx5e resources (See struct mlx5e_priv / mlx5e_txqsq) */
    uint32_t         fPd;
    uint32_t         fTd;
    uint32_t         fTisn;
    uint32_t         fSqn;
    uint32_t         fRqn;
    uint32_t         fTirn;
    uint32_t         fTxCqn;
    uint32_t         fRxCqn;
    uint32_t         fMkeyIndex;
    uint32_t         fLkey;
    uint32_t         fFlowTableId;
    uint32_t         fFlowGroupId;
    uint32_t         fTxDbOffset;     /* SQ doorbell record offset in UAR DB page */
    uint32_t         fRxDbOffset;     /* RQ doorbell record offset */
    uint32_t         fTxCqDbOffset;   /* TX CQ consumer DB offset */
    uint32_t         fRxCqDbOffset;   /* RX CQ consumer DB offset */
    MlxBfreg         fTxBf;
    bool             fTxBfValid;
    void            *fTxCqBuf;        /* TX CQ CQE buffer (kernel VA) */
    void            *fRxCqBuf;        /* RX CQ CQE buffer (kernel VA) */
    IOBufferMemoryDescriptor *fTxCqDesc;
    IODMACommand    *fTxCqMap;
    uint64_t         fTxCqDMA;
    IOBufferMemoryDescriptor *fRxCqDesc;
    IODMACommand    *fRxCqMap;
    uint64_t         fRxCqDMA;
    IOBufferMemoryDescriptor *fTxBufDesc;   /* TX bounce-buffer pool */
    IODMACommand    *fTxBufMap;
    uint64_t         fTxBufDMA;
    IOBufferMemoryDescriptor *fRxBufDesc;   /* RX buffer pool (DMA) */
    IODMACommand *fRxBufMap;               /* RX buffer pool DMA map */
    uint64_t         fRxBufDMA;
    uint16_t         fTxPc;           /* SQ producer counter */
    uint16_t         fTxCc;           /* SQ consumer counter */
    uint16_t         fRxPc;           /* RQ producer counter */
    uint16_t         fRxCc;           /* RQ consumer counter */
    uint32_t         fTxCqCc;
    uint32_t         fRxCqCc;
    uint32_t         fTxArmSn;
    uint32_t         fRxArmSn;
    bool             fFlowRootActive;
    bool             fFlowEntryValid;
    bool             fEnabling;
    bool             fDisabling;
    bool             fNotifierRegistered;
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

    /* TX completion tracking: map a WQE slot to its pending mbuf (See mlx5e_tx_wqe_info.skb) */
    void setWqeMbuf(uint16_t idx, mbuf_t m);
    mbuf_t getWqeMbuf(uint16_t idx);
    void clearWqeMbuf(uint16_t idx);

    uint64_t getWqDMA() const { return fWqDMA; }
    void *getWqBuf() const { return fWqBuf; }
    uint16_t getHead() const { return fHead; }
    void setHead(uint16_t h) { fHead = h; }
    uint32_t *getDbRecord() const { return fDbRecord; }
    void setDbRecord(uint32_t *db) { fDbRecord = db; }
    uint64_t getDbDMA() const { return fDbDMA; }
    void setDbDMA(uint64_t dma) { fDbDMA = dma; }
    IOBufferMemoryDescriptor *getWqDesc() const { return fWqDesc; }

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
    mbuf_t      fWqeMbuf[1 << MLX5E_DEFAULT_LOG_SQ_SIZE];
};

#endif /* MLX_ETHERNET_HPP */
