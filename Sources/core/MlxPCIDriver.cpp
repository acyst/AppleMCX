/*
 * MlxPCIDriver.cpp — PCI binding layer implementation (generic Mellanox family)
 *
 * Ported from: mlx5_core/main.c (probe_one + mlx5_function_setup)
 * Order:
 *   start() → map BAR0 → create command interface → firmware handshake
 *             (ENABLE_HCA/SET_ISSI/CAP/INIT_HCA) → create EQ/UAR → publish nubs
 *             → verbs/Ethernet layers
 */
#include "MlxPCIDriver.hpp"
#include "MlxRoCE.hpp"
#include "MlxCmd.hpp"
#include "MlxEQ.hpp"
#include "MlxUAR.hpp"
#include "MlxRegs.hpp"
#include "MlxKernelCompat.hpp"
#include "MlxP1Encoding.hpp"

#include <stdio.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSDictionary.h>
#include <mach/vm_param.h>

#define super IOService
OSDefineMetaClassAndStructors(MlxPCIDriver, IOService)

#ifndef APPLEMCX_ENABLE_UNSAFE_ROCE
#define APPLEMCX_ENABLE_UNSAFE_ROCE 0
#endif

/* ---- Lifecycle ---- */

bool MlxPCIDriver::init(OSDictionary *properties)
{
    if (!super::init(properties))
        return false;

    fPci = NULL;
    fBar0Map = NULL;
    fCmd = NULL;
    fEQ = NULL;
    fUAR = NULL;
    fHCA = NULL;
    fHealth = NULL;
    fDMA = NULL;
    fFwPages = NULL;
    fRoCE = NULL;
    fEth = NULL;
    fIssi = 0;
    fHcaEnabled = false;
    fHcaInitialized = false;
    fRuntimePagesStarted = false;
    fStopping = false;
    fDmaQuarantined = false;
    return true;
}

bool MlxPCIDriver::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    /* Get the PCI device */
    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) {
        IOLog("MlxPCIDriver: provider is not IOPCIDevice\n");
        return false;
    }
    fPci->retain();
    fDeviceId = fPci->configRead16(kIOPCIConfigDeviceID);

    /* Allocate a global device index (distinguishes mlx5_0/mlx5_1/... for multiple devices, See dev.c:318 adev_idx) */
    static uint32_t sDevIdx = 0;
    fDevIdx = sDevIdx++;
    snprintf(fDevName, sizeof(fDevName), "mlx5_%u", fDevIdx);

    /* Enable bus mastering + memory space (See main.c:1080 mlx5_pci_init) */
    fPci->setBusMasterEnable(true);
    fPci->setMemoryEnable(true);

    /* Map BAR0 (init segment + command interface) */
    fBar0Map = fPci->mapDeviceMemoryWithIndex(0);
    if (!fBar0Map) {
        IOLog("MlxPCIDriver: BAR0 mapping failed (device=0x%04x)\n", fDeviceId);
        cleanup();
        return false;
    }

    /* Hardware model dispatch */
    fHCA = MlxHCALoader::create(fDeviceId);
    if (!fHCA) {
        IOLog("MlxPCIDriver: unsupported device 0x%04x (supported: ConnectX-4~8)\n",
              fDeviceId);
        cleanup();
        return false;
    }
    fHCA->attachCore(this);
    MlxVendorInfo &vendor = fHCA->mutableVendor();
    vendor.vendorId = fPci->configRead16(kIOPCIConfigVendorID);
    vendor.deviceId = fDeviceId;
    vendor.revision = fPci->configRead16(kIOPCIConfigRevisionID);

    /* Command interface (foundation) */
    fCmd = OSTypeAlloc(MlxCmd);
    if (!fCmd || !fCmd->init(this, fBar0Map, 4096)) {
        IOLog("MlxPCIDriver: command interface init failed\n");
        cleanup();
        return false;
    }

    fFwPages = OSTypeAlloc(MlxFwPages);
    if (!fFwPages || !fFwPages->init(this, false)) {
        IOLog("MlxPCIDriver: firmware page manager init failed\n");
        if (fFwPages) {
            fFwPages->release();
            fFwPages = NULL;
        }
        cleanup();
        return false;
    }

    /* Firmware handshake sequence (See mlx5_function_setup, main.c:1361) */
    if (!fwInit()) {
        IOLog("MlxPCIDriver: firmware init failed\n");
        cleanup();
        return false;
    }

    /* Allocate the boot UAR before any EQ references its uar_page. */
    fUAR = OSTypeAlloc(MlxUAR);
    if (!fUAR || !fUAR->init(this)) {
        IOLog("MlxPCIDriver: UAR init failed\n");
        cleanup();
        return false;
    }
    /* Allocate the boot UAR (used for EQ/CQ uar_page, See uar.c:165 mlx5_get_uars_page)
     * allocUar internally records the first UAR as the boot index */
    MlxUarAlloc bootUar;
    if (fUAR->allocUar(&bootUar) != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: boot UAR allocation failed\n");
        cleanup();
        return false;
    }

    /* Allocate the DB record page (user-space post_send updates the queue head pointers) */
    if (fUAR->allocDbRecord() != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: DB record allocation failed\n");
        cleanup();
        return false;
    }

    int interruptType = 0;
    if (fPci->configureInterrupts(kIOInterruptTypePCIMessagedX, 2, 2) !=
        kIOReturnSuccess ||
        fPci->getInterruptType(0, &interruptType) != kIOReturnSuccess ||
        !(interruptType & kIOInterruptTypePCIMessagedX) ||
        fPci->getInterruptType(1, &interruptType) != kIOReturnSuccess ||
        !(interruptType & kIOInterruptTypePCIMessagedX)) {
        IOLog("MlxPCIDriver: control/completion interrupt sources unavailable\n");
        cleanup();
        return false;
    }
    fEQ = OSTypeAlloc(MlxEQ);
    if (!fEQ || !fEQ->init(this, 1) ||
        fEQ->createAsyncEqs() != kIOReturnSuccess ||
        fEQ->createCompEqs() != kIOReturnSuccess ||
        fFwPages->startRuntime(fEQ) != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: EQ initialization failed\n");
        cleanup();
        return false;
    }
    fRuntimePagesStarted = true;
    if (fEQ->setupInterrupts() != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: interrupt setup failed\n");
        cleanup();
        return false;
    }

    /* Health polling */
    fHealth = OSTypeAlloc(MlxHealth);
    if (!fHealth || !fHealth->init(this)) {
        IOLog("MlxPCIDriver: health monitoring init failed\n");
        cleanup();
        return false;
    }
    if (!fHealth->start(1000000)) {
        IOLog("MlxPCIDriver: health timer setup failed\n");
        cleanup();
        return false;
    }

    /* DMA mapping service (pins user memory on the data path) */
    fDMA = OSTypeAlloc(MlxDMA);
    if (!fDMA || !fDMA->init()) {
        IOLog("MlxPCIDriver: DMA service init failed\n");
        cleanup();
        return false;
    }

    /* Publish child services (triggers the verbs/Ethernet layer probe) */
    if (!publishNubs()) {
        IOLog("MlxPCIDriver: nub publishing failed\n");
        cleanup();
        return false;
    }

    /* Register the service for user-space IOServiceOpen */
    registerService();

    IOLog("MlxPCIDriver: device ready (device=0x%04x, fw_rev=%08x)\n",
           fDeviceId, mlxMMIORead32BE(fBar0Map, offsetof(struct MlxInitSeg, fw_rev)));
    return true;
}

void MlxPCIDriver::stop(IOService *provider)
{
    cleanup();
    super::stop(provider);
}

void MlxPCIDriver::cleanup()
{
    if (fStopping)
        return;
    fStopping = true;
    if (fHealth) {
        fHealth->stop();
        fHealth->release();
        fHealth = NULL;
    }
    if (fEQ)
        fEQ->disableInterrupts();
    if (fFwPages && fRuntimePagesStarted) {
        fFwPages->stopRuntimeAndDrain();
        fRuntimePagesStarted = false;
    }
    bool eqDestroyed = !fEQ || fEQ->shutdown();
    bool teardownOk = true;
    if (fHcaInitialized && fCmd && fCmd->isUp()) {
        teardownOk = teardownHca();
        if (teardownOk)
            fHcaInitialized = false;
        else
            fDmaQuarantined = true;
    }
    if (fFwPages && fHcaEnabled && fCmd && fCmd->isUp() &&
        !fCmd->isQuarantined() && teardownOk &&
        fFwPages->hasOutstandingPages()) {
        if (fFwPages->reclaimAll(5000) != kIOReturnSuccess)
            fDmaQuarantined = true;
    }
    if (fHcaEnabled && fCmd && fCmd->isUp()) {
        if (!disableHca())
            fDmaQuarantined = true;
        else
            fHcaEnabled = false;
    }
    if (fDmaQuarantined && fPci)
        fPci->setBusMasterEnable(false);
    bool gracefulBoundary = eqDestroyed && teardownOk && !fDmaQuarantined &&
                            (!fCmd || !fCmd->isQuarantined()) &&
                            (!fFwPages || !fFwPages->hasOutstandingPages());
    bool busMasterDisabled = disableBusMasterAndVerify();
    if (!busMasterDisabled) {
        IOLog("MlxPCIDriver: bus-master-off not verified; DMA resources quarantined\n");
        return;
    }
    if (!gracefulBoundary) {
        IOLog("MlxPCIDriver: teardown was not a trusted DMA drain; retaining mappings\n");
        return;
    }
    if (fFwPages) {
        fFwPages->releaseAfterDmaBoundary();
        fFwPages->release();
        fFwPages = NULL;
    }
    if (!eqDestroyed && fEQ)
        fEQ->markHardwareStopped();
    if (fDMA) {
        fDMA->release();
        fDMA = NULL;
    }
    if (fEQ) {
        fEQ->release();
        fEQ = NULL;
    }
    if (fUAR) {
        fUAR->release();
        fUAR = NULL;
    }
    if (fCmd) {
        fCmd->release();
        fCmd = NULL;
    }
    if (fHCA) {
        delete fHCA;
        fHCA = NULL;
    }
    if (fBar0Map) {
        fBar0Map->release();
        fBar0Map = NULL;
    }
    if (fPci) {
        fPci->setMemoryEnable(false);
        fPci->release();
        fPci = NULL;
    }
}

void MlxPCIDriver::free()
{
    cleanup();
    super::free();
}

/* ---- Firmware initialization sequence ---- */

bool MlxPCIDriver::fwInit()
{
    /* 1. Wait for firmware to leave pre-init (See wait_fw_init, main.c:281-307) */
    bool firmwareReady = false;
    for (int i = 0; i < 100; i++) {
        uint32_t init = mlxMMIORead32BE(
            fBar0Map, offsetof(struct MlxInitSeg, initializing));
        if (!(init & (1u << 31))) {
            firmwareReady = true;
            break;
        }
        IOSleep(10);
    }
    if (!firmwareReady) {
        IOLog("MlxPCIDriver: firmware remained in pre-initializing state\n");
        return false;
    }

    /* 2. ENABLE_HCA (See mlx5_core_enable_hca, main.c:1401) */
    if (!enableHca()) {
        IOLog("MlxPCIDriver: ENABLE_HCA failed\n");
        return false;
    }
    fHcaEnabled = true;

    /* 3. Negotiate ISSI (See mlx5_core_set_issi, main.c:1407) */
    if (!setIssi()) {
        IOLog("MlxPCIDriver: SET_ISSI failed\n");
        return false;
    }

    if (fFwPages->satisfyStartupPages(kMlxFwPageBoot) !=
        kIOReturnSuccess) {
        IOLog("MlxPCIDriver: boot page provisioning failed\n");
        return false;
    }

    /* 4. Set capabilities (SET_HCA_CAP, including the RoCE switch) */
    if (!setHcaCaps()) {
        IOLog("MlxPCIDriver: SET_HCA_CAP failed\n");
        return false;
    }

    if (fFwPages->satisfyStartupPages(kMlxFwPageInit) !=
        kIOReturnSuccess) {
        IOLog("MlxPCIDriver: init page provisioning failed\n");
        return false;
    }

    /* 5. INIT_HCA (passes sw_owner_id) */
    if (!initHca()) {
        IOLog("MlxPCIDriver: INIT_HCA failed\n");
        return false;
    }
    fHcaInitialized = true;

    /* 6. Read back the final capabilities */
    if (!queryHcaCaps()) {
        IOLog("MlxPCIDriver: QUERY_HCA_CAP failed\n");
        return false;
    }

    /* 7. RoCE capability negotiation */
    negotiateRoceCap();

    return true;
}

bool MlxPCIDriver::enableHca()
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_ENABLE_HCA);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_ENABLE_HCA };
    return fCmd->exec(&cmd, 5000) == kIOReturnSuccess;
}

bool MlxPCIDriver::setIssi()
{
    /* See mlx5_core_set_issi (main.c:982)
     * Steps: QUERY_ISSI → if ISSI=1 is supported then SET_ISSI=1, otherwise fall back to ISSI=0
     * Note: older ConnectX-4 firmware may only support ISSI 0 (no QUERY_ISSI support) */
    uint8_t in[16] = {};
    uint8_t out[112] = {};

    /* 1. QUERY_ISSI */
    OSWriteBigInt16(in, 0, MLX_CMD_OP_QUERY_ISSI);
    MlxCmdInOut qcmd = { in, sizeof(in), out, sizeof(out),
                         MLX_CMD_OP_QUERY_ISSI };
    kern_return_t kr = fCmd->exec(&qcmd, 5000);
    if (kr != kIOReturnSuccess) {
        /* Firmware does not support QUERY_ISSI → ISSI=0 (See main.c:1001) */
        fIssi = 0;
        IOLog("MlxPCIDriver: firmware does not support QUERY_ISSI, falling back to ISSI=0 (old firmware/CX-4)\n");
        return true;
    }

    /* 2. Check supported_issi_dw0 bit1 */
    uint32_t supIssi = static_cast<uint32_t>(mlxGetBits(out, 0x360, 32));
    if (supIssi & (1u << 1)) {
        /* 3. SET_ISSI = 1 */
        uint8_t setIn[16] = {};
        uint8_t setOut[16] = {};
        OSWriteBigInt16(setIn, 0, MLX_CMD_OP_SET_ISSI);
        mlxSetBits(setIn, 0x50, 16, 1);        /* current_issi = 1 */
        MlxCmdInOut scmd = { setIn, sizeof(setIn), setOut, sizeof(setOut),
                             MLX_CMD_OP_SET_ISSI };
        kr = fCmd->exec(&scmd, 5000);
        if (kr != kIOReturnSuccess) {
            fIssi = 0;
            IOLog("MlxPCIDriver: SET_ISSI=1 failed, falling back to ISSI=0\n");
        } else {
            fIssi = 1;
        }
        IOLog("MlxPCIDriver: ISSI = %u\n", fIssi);
        return true;
    }

    /* Only ISSI=0 is supported (common on ConnectX-4) */
    fIssi = 0;
    IOLog("MlxPCIDriver: firmware only supports ISSI=0\n");
    return true;
}

bool MlxPCIDriver::setHcaCaps()
{
    uint8_t *maxCap = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *curCap = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *in = static_cast<uint8_t *>(IOMallocZero(MLX_P1_SET_HCA_CAP_IN_BYTES));
    if (!maxCap || !curCap || !in) {
        if (maxCap) IOFree(maxCap, MLX_P1_HCA_CAP_BYTES);
        if (curCap) IOFree(curCap, MLX_P1_HCA_CAP_BYTES);
        if (in) IOFree(in, MLX_P1_SET_HCA_CAP_IN_BYTES);
        return false;
    }
    bool ok = queryHcaCap(MLX_P1_CAP_GENERAL, MLX_P1_CAP_MAX, maxCap) &&
              queryHcaCap(MLX_P1_CAP_GENERAL, MLX_P1_CAP_CURRENT, curCap);
    if (ok) {
        memcpy(in + MLX_P1_CMD_HEADER_BYTES, curCap, 256);
        mlxSetBits(in, 0x00, 16, MLX_CMD_OP_SET_HCA_CAP);
        mlxSetBits(in, 0x30, 16, MLX_P1_CAP_GENERAL << 1);
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x210, 2, 0);
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x20f, 1, 1);
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x145, 1,
                   mlxGetBits(maxCap, 0x145, 1));
        uint16_t logUarPageSize = PAGE_SHIFT > 12 ? PAGE_SHIFT - 12 : 0;
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x240, 1,
                   PAGE_SIZE > 4096 && mlxGetBits(maxCap, 0x240, 1));
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x490, 16,
                   logUarPageSize);
        uint8_t out[16] = {};
        MlxCmdInOut cmd = { in, MLX_P1_SET_HCA_CAP_IN_BYTES,
                            out, sizeof(out), MLX_CMD_OP_SET_HCA_CAP };
        ok = fCmd->exec(&cmd, 5000) == kIOReturnSuccess;
    }
    IOFree(in, MLX_P1_SET_HCA_CAP_IN_BYTES);
    IOFree(curCap, MLX_P1_HCA_CAP_BYTES);
    IOFree(maxCap, MLX_P1_HCA_CAP_BYTES);
    return ok;
}

bool MlxPCIDriver::initHca()
{
    uint8_t in[MLX_P1_INIT_HCA_IN_BYTES] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_INIT_HCA);
    /* sw_owner_id: 24 bytes, zeroed in MVP */
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_INIT_HCA };
    return fCmd->exec(&cmd, 5000) == kIOReturnSuccess;
}

bool MlxPCIDriver::teardownHca()
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_TEARDOWN_HCA);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_TEARDOWN_HCA };
    return fCmd->exec(&cmd, 5000) == kIOReturnSuccess;
}

bool MlxPCIDriver::disableHca()
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_DISABLE_HCA);
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_DISABLE_HCA };
    return fCmd->exec(&cmd, 5000) == kIOReturnSuccess;
}

bool MlxPCIDriver::queryHcaCaps()
{
    uint8_t *general = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *roce = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *flow = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *ethernet = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    if (!general || !roce || !flow || !ethernet) {
        if (general) IOFree(general, MLX_P1_HCA_CAP_BYTES);
        if (roce) IOFree(roce, MLX_P1_HCA_CAP_BYTES);
        if (flow) IOFree(flow, MLX_P1_HCA_CAP_BYTES);
        if (ethernet) IOFree(ethernet, MLX_P1_HCA_CAP_BYTES);
        return false;
    }
    bool ok = queryHcaCap(MLX_P1_CAP_GENERAL, MLX_P1_CAP_CURRENT, general);
    MlxP1GeneralCaps parsed = {};
    MlxP1RoceCaps parsedRoce = {};
    MlxP1FlowCaps parsedFlow = {};
    MlxP1EthernetCaps parsedEthernet = {};
    ok = ok && mlxP1ParseGeneralCaps(general, MLX_P1_HCA_CAP_BYTES, &parsed);
    bool haveRoce = parsed.roce &&
        queryHcaCap(MLX_P1_CAP_ROCE, MLX_P1_CAP_CURRENT, roce) &&
        mlxP1ParseRoceCaps(roce, MLX_P1_HCA_CAP_BYTES, &parsedRoce);
    bool haveFlow = parsed.nicFlowTable &&
        queryHcaCap(MLX_P1_CAP_FLOW_TABLE, MLX_P1_CAP_CURRENT, flow) &&
        mlxP1ParseFlowCaps(flow, MLX_P1_HCA_CAP_BYTES, &parsedFlow);
    bool haveEthernet = parsed.ethNetOffloads &&
        queryHcaCap(MLX_P1_CAP_ETHERNET_OFFLOADS, MLX_P1_CAP_CURRENT,
                    ethernet) &&
        mlxP1ParseEthernetCaps(ethernet, MLX_P1_HCA_CAP_BYTES,
                               &parsedEthernet);
    if (!ok) {
        IOFree(flow, MLX_P1_HCA_CAP_BYTES);
        IOFree(roce, MLX_P1_HCA_CAP_BYTES);
        IOFree(general, MLX_P1_HCA_CAP_BYTES);
        IOFree(ethernet, MLX_P1_HCA_CAP_BYTES);
        return false;
    }
    MlxHcaCaps &caps = fHCA->mutableCaps();
    memset(&caps, 0, sizeof(caps));
    caps.fwRev = mlxMMIORead32BE(
        fBar0Map, offsetof(struct MlxInitSeg, fw_rev));
    caps.cmdifRev = fCmd->cmdifRev();
    caps.maxQp = mlxP1LogResourceSize(parsed.logMaxQp);
    caps.maxCq = mlxP1LogResourceSize(parsed.logMaxCq);
    caps.maxMr = mlxP1LogResourceSize(parsed.logMaxMkey);
    caps.portType = parsed.portType;
    caps.numPorts = parsed.numPorts;
    caps.roce = haveRoce;
    caps.uar4k = parsed.uar4k;
    caps.logUarPageSize = static_cast<uint16_t>(
        mlxGetBits(general, 0x490, 16));
    caps.logBfRegSize = parsed.bf ? parsed.logBfRegSize : 0;
    caps.roceRwSupported = parsed.roceRwSupported;
    caps.roceMaxGid = haveRoce ? parsedRoce.addressTableSize : 0;
    caps.roceVersions = haveRoce ?
        mlxP1RoceVersionsForAbi(parsedRoce.versions) : 0;
    caps.roceDstUdpPort = haveRoce ? parsedRoce.destinationUdpPort : 0;
    caps.roceMinSrcUdpPort = haveRoce ? parsedRoce.minimumSourceUdpPort : 0;
    caps.swRoceSrcUdpPort = haveRoce && parsedRoce.sourceUdpPortWritable;
    caps.nicFlowTable = haveFlow && parsedFlow.nicRx && parsedFlow.nicTx;
    caps.ethNetOffloads = haveEthernet && parsedEthernet.checksum &&
                          parsedEthernet.vlan;
    caps.ibSupported = caps.portType == MLX_PORT_TYPE_IB;
    caps.ibMaxPkeys = static_cast<uint16_t>(
        mlxP1PkeyTableSize(parsed.pkeyTableEncoding));
    if (!caps.numPorts || !caps.maxQp || !caps.maxCq || !caps.maxMr) {
        IOFree(flow, MLX_P1_HCA_CAP_BYTES);
        IOFree(roce, MLX_P1_HCA_CAP_BYTES);
        IOFree(general, MLX_P1_HCA_CAP_BYTES);
        IOFree(ethernet, MLX_P1_HCA_CAP_BYTES);
        return false;
    }
    IOFree(flow, MLX_P1_HCA_CAP_BYTES);
    IOFree(roce, MLX_P1_HCA_CAP_BYTES);
    IOFree(general, MLX_P1_HCA_CAP_BYTES);
    IOFree(ethernet, MLX_P1_HCA_CAP_BYTES);
    IOLog("MlxPCIDriver: capability query complete\n");
    return true;
}

bool MlxPCIDriver::queryHcaCap(uint16_t type, uint16_t mode,
                               uint8_t *capability)
{
    if (!capability)
        return false;
    uint8_t in[MLX_P1_QUERY_HCA_CAP_IN_BYTES] = {};
    uint8_t *out = static_cast<uint8_t *>(
        IOMallocZero(MLX_P1_QUERY_HCA_CAP_OUT_BYTES));
    if (!out)
        return false;
    mlxP1EncodeQueryHcaCap(in, sizeof(in), type, mode);
    MlxCmdInOut cmd = { in, sizeof(in), out,
                        MLX_P1_QUERY_HCA_CAP_OUT_BYTES,
                        MLX_CMD_OP_QUERY_HCA_CAP };
    IOReturn kr = fCmd->exec(&cmd, 5000);
    if (kr == kIOReturnSuccess)
        memcpy(capability, out + MLX_P1_CMD_HEADER_BYTES,
               MLX_P1_HCA_CAP_BYTES);
    IOFree(out, MLX_P1_QUERY_HCA_CAP_OUT_BYTES);
    return kr == kIOReturnSuccess;
}

bool MlxPCIDriver::negotiateRoceCap()
{
    /* See handle_hca_cap_roce (main.c:824-847)
     * Full implementation: read roce_cap, check RoCE v2 support */
    return true;
}

kern_return_t MlxPCIDriver::exec(uint32_t opcode, const void *in,
                                 uint32_t inSize, void *out,
                                 uint32_t outSize, uint32_t timeoutMs)
{
    if (!fCmd)
        return kIOReturnNotReady;
    MlxCmdInOut cmd = { in, inSize, out, outSize, opcode };
    return fCmd->exec(&cmd, timeoutMs);
}

bool MlxPCIDriver::publishNubs()
{
    /* Publish child-service properties, triggering the verbs layer (MlxRoCE)
     * and the Ethernet layer (MlxEthernetDriver)
     * See mlx5_register_device → add_adev (dev.c:306)
     * deviceName: multi-device identifier (mlx5_0/mlx5_1), enumerated by name in user space */
    if (rocePublicationAllowed())
        setProperty("mlx_rdma", true);
    else
        removeProperty("mlx_rdma");
    if (fHCA->caps().isEthernet() && fHCA->caps().ethNetOffloads &&
        fHCA->caps().nicFlowTable && fEQ && fEQ->getNumCompEqs() > 0)
        setProperty("mlx_eth", true);
    else
        removeProperty("mlx_eth");
    setProperty("deviceName", fDevName);
    return true;
}

void MlxPCIDriver::enterDmaQuarantine(uint32_t reason)
{
    fDmaQuarantined = true;
    removeProperty("mlx_eth");
    removeProperty("mlx_rdma");
    if (fPci)
        fPci->setBusMasterEnable(false);
    IOLog("MlxPCIDriver: DMA quarantine requested (reason=%u)\n", reason);
}

bool MlxPCIDriver::disableBusMasterAndVerify()
{
    if (!fPci)
        return true;
    fPci->setBusMasterEnable(false);
    uint16_t command = fPci->configRead16(kIOPCIConfigCommand);
    return (command & 0x0004) == 0;
}

bool MlxPCIDriver::rocePublicationAllowed() const
{
#if APPLEMCX_ENABLE_UNSAFE_ROCE
    return fHCA && fHCA->caps().roce && fEQ && fDMA && fUAR;
#else
    return false;
#endif
}
