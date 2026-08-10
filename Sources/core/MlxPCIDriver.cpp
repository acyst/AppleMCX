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

#include <stdio.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSDictionary.h>

#define super IOService
OSDefineMetaClassAndStructors(MlxPCIDriver, IOService)

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
    fRoCE = NULL;
    fEth = NULL;
    fIssi = 0;
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
        return false;
    }

    /* Hardware model dispatch */
    fHCA = MlxHCALoader::create(fDeviceId);
    if (!fHCA) {
        IOLog("MlxPCIDriver: unsupported device 0x%04x (supported: ConnectX-4~8)\n",
              fDeviceId);
        return false;
    }
    /* Bind the core (loader only dispatches the type; each model exports its attach) */
    extern void mlx_hca_attach_core(MlxHCA *hca, MlxPCIDriver *core);
    extern void mlx_hca_attach_core_cx4(MlxHCA *hca, MlxPCIDriver *core);
    extern void mlx_hca_attach_core_cx6(MlxHCA *hca, MlxPCIDriver *core);
    extern void mlx_hca_attach_core_cx7(MlxHCA *hca, MlxPCIDriver *core);
    mlx_hca_attach_core(fHCA, this);
    mlx_hca_attach_core_cx4(fHCA, this);
    mlx_hca_attach_core_cx6(fHCA, this);
    mlx_hca_attach_core_cx7(fHCA, this);

    /* Command interface (foundation) */
    fCmd = OSTypeAlloc(MlxCmd);
    if (!fCmd || !fCmd->init(this, fBar0Map, 4096)) {
        IOLog("MlxPCIDriver: command interface init failed\n");
        return false;
    }

    /* Firmware handshake sequence (See mlx5_function_setup, main.c:1361) */
    if (!fwInit()) {
        IOLog("MlxPCIDriver: firmware init failed\n");
        return false;
    }

    /* Event queues + UAR */
    fEQ = OSTypeAlloc(MlxEQ);
    if (!fEQ || !fEQ->init(this, 4)) {
        IOLog("MlxPCIDriver: EQ init failed\n");
        return false;
    }
    /* Full EQ init chain (See eq.c:696 create_async_eqs + create_comp_eqs):
     *   - 3 async EQs (cmd/async/pages)
     *   - N completion EQs (one per MSI-X vector)
     *   - MSI-X interrupt registration (IOInterruptEventSource) */
    if (fEQ->createAsyncEqs() != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: async EQ creation failed\n");
        return false;
    }
    if (fEQ->createCompEqs() != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: completion EQ creation failed\n");
        return false;
    }
    if (fEQ->setupInterrupts() != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: MSI-X interrupt registration failed\n");
        return false;
    }

    fUAR = OSTypeAlloc(MlxUAR);
    if (!fUAR || !fUAR->init(this)) {
        IOLog("MlxPCIDriver: UAR init failed\n");
        return false;
    }
    /* Allocate the boot UAR (used for EQ/CQ uar_page, See uar.c:165 mlx5_get_uars_page)
     * allocUar internally records the first UAR as the boot index */
    MlxUarAlloc bootUar;
    if (fUAR->allocUar(&bootUar) != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: boot UAR allocation failed\n");
        return false;
    }

    /* Allocate the DB record page (user-space post_send updates the queue head pointers) */
    if (fUAR->allocDbRecord() != kIOReturnSuccess) {
        IOLog("MlxPCIDriver: DB record allocation failed\n");
        return false;
    }

    /* Health polling */
    fHealth = OSTypeAlloc(MlxHealth);
    if (!fHealth || !fHealth->init(this)) {
        IOLog("MlxPCIDriver: health monitoring init failed\n");
        return false;
    }
    fHealth->start(1000000);   /* 1 second interval */

    /* DMA mapping service (pins user memory on the data path) */
    fDMA = OSTypeAlloc(MlxDMA);
    if (!fDMA || !fDMA->init()) {
        IOLog("MlxPCIDriver: DMA service init failed\n");
        return false;
    }

    /* Publish child services (triggers the verbs/Ethernet layer probe) */
    if (!publishNubs()) {
        IOLog("MlxPCIDriver: nub publishing failed\n");
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
    if (fDMA) {
        fDMA->release();
        fDMA = NULL;
    }
    if (fCmd) {
        fCmd->release();
        fCmd = NULL;
    }
    if (fEQ) {
        fEQ->release();
        fEQ = NULL;
    }
    if (fUAR) {
        fUAR->freeDbRecord();
        fUAR->release();
        fUAR = NULL;
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
        fPci->release();
        fPci = NULL;
    }
    super::stop(provider);
}

void MlxPCIDriver::free()
{
    super::free();
}

/* ---- Firmware initialization sequence ---- */

bool MlxPCIDriver::fwInit()
{
    /* 1. Wait for firmware to leave pre-init (See wait_fw_init, main.c:281-307) */
    for (int i = 0; i < 100; i++) {
        uint32_t init = mlxMMIORead32BE(
            fBar0Map, offsetof(struct MlxInitSeg, initializing));
        if (!(init & (1u << 31)))
            break;
        IOSleep(10);
    }

    /* 2. ENABLE_HCA (See mlx5_core_enable_hca, main.c:1401) */
    if (!enableHca()) {
        IOLog("MlxPCIDriver: ENABLE_HCA failed\n");
        return false;
    }

    /* 3. Negotiate ISSI (See mlx5_core_set_issi, main.c:1407) */
    if (!setIssi()) {
        IOLog("MlxPCIDriver: SET_ISSI failed\n");
        return false;
    }

    /* 4. Set capabilities (SET_HCA_CAP, including the RoCE switch) */
    if (!setHcaCaps()) {
        IOLog("MlxPCIDriver: SET_HCA_CAP failed\n");
        return false;
    }

    /* 5. INIT_HCA (passes sw_owner_id) */
    if (!initHca()) {
        IOLog("MlxPCIDriver: INIT_HCA failed\n");
        return false;
    }

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
    uint8_t out[64] = {};

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
    uint32_t supIssi = OSReadBigInt32(out, 0x40) & 0xFFFFFFFF;
    if (supIssi & (1u << 1)) {
        /* 3. SET_ISSI = 1 */
        uint8_t setIn[16] = {};
        uint8_t setOut[16] = {};
        OSWriteBigInt16(setIn, 0, MLX_CMD_OP_SET_ISSI);
        OSWriteBigInt32(setIn, 4, 1);          /* current_issi = 1 */
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
    /* MVP: capability negotiation is handled after queryHcaCaps reads back;
     * the full implementation (SET_HCA_CAP) is completed in late phase P0 per handle_hca_cap */
    return true;
}

bool MlxPCIDriver::initHca()
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_INIT_HCA);
    /* sw_owner_id: 24 bytes, zeroed in MVP */
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_INIT_HCA };
    return fCmd->exec(&cmd, 5000) == kIOReturnSuccess;
}

bool MlxPCIDriver::queryHcaCaps()
{
    /* QUERY_HCA_CAP → parse fHCA->caps()
     * See mlx5_query_hca_caps (main.c:1455) */
    uint8_t in[32] = {};
    uint8_t out[1024] = {};
    OSWriteBigInt16(in, 0, MLX_CMD_OP_QUERY_HCA_CAP);
    OSWriteBigInt16(in, 2, 0);          /* op_mod: query current */
    MlxCmdInOut cmd = { in, sizeof(in), out, sizeof(out),
                        MLX_CMD_OP_QUERY_HCA_CAP };
    if (fCmd->exec(&cmd, 5000) != kIOReturnSuccess)
        return false;

    /* Parse key fields (capability bit layout See mlx5_ifc.h:1505 cmd_hca_cap_bits) */
    IOLog("MlxPCIDriver: capability query complete\n");
    return true;
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
    setProperty("mlx_rdma", true);
    setProperty("mlx_eth", true);
    setProperty("deviceName", fDevName);
    return true;
}
