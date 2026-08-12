#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "MlxP1Encoding.hpp"

static void testOutbox()
{
    uint8_t out[16] = {};
    mlxSetBits(out, 0x00, 8, 6);
    mlxSetBits(out, 0x20, 32, 0x12345678);
    MlxP1OutboxStatus status = {};
    assert(mlxP1ParseOutbox(out, sizeof(out), &status));
    assert(status.status == 6);
    assert(status.syndrome == 0x12345678);
    assert(!mlxP1ParseOutbox(out, 7, &status));
}

static void testCapabilities()
{
    uint8_t in[MLX_P1_QUERY_HCA_CAP_IN_BYTES] = {};
    assert(mlxP1EncodeQueryHcaCap(in, sizeof(in), MLX_P1_CAP_ROCE,
                                  MLX_P1_CAP_CURRENT));
    assert(mlxGetBits(in, 0x00, 16) == 0x100);
    assert(mlxGetBits(in, 0x30, 16) == 9);

    uint8_t cap[MLX_P1_HCA_CAP_BYTES] = {};
    mlxSetBits(cap, 0x9b, 5, 18);
    mlxSetBits(cap, 0xdb, 5, 16);
    mlxSetBits(cap, 0xea, 6, 20);
    mlxSetBits(cap, 0x170, 16, 4);
    mlxSetBits(cap, 0x190, 16, 2);
    mlxSetBits(cap, 0x1a6, 1, 1);
    mlxSetBits(cap, 0x1b6, 2, 1);
    mlxSetBits(cap, 0x1b8, 8, 2);
    mlxSetBits(cap, 0x21b, 1, 1);
    mlxSetBits(cap, 0x21c, 1, 1);
    mlxSetBits(cap, 0x240, 1, 1);
    mlxSetBits(cap, 0x260, 1, 1);
    mlxSetBits(cap, 0x26b, 5, 6);
    mlxSetBits(cap, 0x3a1, 1, 1);
    MlxP1GeneralCaps general = {};
    assert(mlxP1ParseGeneralCaps(cap, sizeof(cap), &general));
    assert(general.logMaxQp == 18 && general.logMaxCq == 16);
    assert(general.logMaxMkey == 20 && general.portType == 1);
    assert(general.numPorts == 2 && general.roce && general.roceRwSupported);
    assert(general.uar4k && general.bf && general.logBfRegSize == 6);
    assert(general.nicFlowTable && general.ethNetOffloads);
    assert(mlxP1GidTableSize(general.gidTableEncoding) == 128);
    assert(mlxP1PkeyTableSize(general.pkeyTableEncoding) == 512);
    assert(mlxP1GidTableSize(5) == 0);

    memset(cap, 0, sizeof(cap));
    mlxSetBits(cap, 0x04, 1, 1);
    mlxSetBits(cap, 0x98, 8, 5);
    mlxSetBits(cap, 0xb0, 16, 4791);
    mlxSetBits(cap, 0xd0, 16, 0xc000);
    mlxSetBits(cap, 0xf0, 16, 128);
    MlxP1RoceCaps roce = {};
    assert(mlxP1ParseRoceCaps(cap, sizeof(cap), &roce));
    assert(roce.sourceUdpPortWritable && roce.versions == 5);
    assert(roce.destinationUdpPort == 4791);
    assert(roce.minimumSourceUdpPort == 0xc000);
    assert(roce.addressTableSize == 128);
    assert(mlxP1RoceVersionsForAbi(roce.versions) == 3);

    memset(cap, 0, sizeof(cap));
    mlxSetBits(cap, 0x200, 1, 1);
    mlxSetBits(cap, 0x800, 1, 1);
    MlxP1FlowCaps flow = {};
    assert(mlxP1ParseFlowCaps(cap, sizeof(cap), &flow));
    assert(flow.nicRx && flow.nicTx);

    memset(cap, 0, sizeof(cap));
    mlxSetBits(cap, 0x00, 1, 1);
    mlxSetBits(cap, 0x01, 1, 1);
    MlxP1EthernetCaps ethernet = {};
    assert(mlxP1ParseEthernetCaps(cap, sizeof(cap), &ethernet));
    assert(ethernet.checksum && ethernet.vlan);
}

static void testPages()
{
    uint8_t query[16] = {};
    assert(mlxP1EncodeQueryPages(query, sizeof(query), 1, true));
    assert(mlxGetBits(query, 0x00, 16) == 0x107);
    assert(mlxGetBits(query, 0x30, 16) == 1);
    assert(mlxGetBits(query, 0x40, 1) == 1);

    const uint64_t pages[3] = { 0x1000, 0x9000, 0x12000 };
    uint8_t give[40] = {};
    assert(mlxP1EncodeManagePages(give, sizeof(give), MLX_P1_PAGES_GIVE,
                                  0x1234, false, pages, 3));
    assert(mlxGetBits(give, 0x00, 16) == 0x108);
    assert(mlxGetBits(give, 0x30, 16) == MLX_P1_PAGES_GIVE);
    assert(mlxGetBits(give, 0x50, 16) == 0x1234);
    assert(mlxGetBits(give, 0x60, 32) == 3);
    for (uint32_t i = 0; i < 3; i++)
        assert(mlxGetBits(give, 0x80 + i * 64, 64) == pages[i]);

    uint8_t take[16] = {};
    assert(mlxP1EncodeManagePages(take, sizeof(take), MLX_P1_PAGES_TAKE,
                                  7, true, NULL, 512));
    assert(mlxGetBits(take, 0x30, 16) == MLX_P1_PAGES_TAKE);
    assert(mlxGetBits(take, 0x40, 1) == 1);
    assert(mlxGetBits(take, 0x60, 32) == 512);
    assert(mlxP1ManagePagesSize(512) == 4112);
    assert(mlxP1ManagePagesSize(513) == 0);
}

static void testEventMasks()
{
    uint64_t mask[4] = {};
    assert(mlxP1SetEvent(mask, 0));
    assert(mlxP1SetEvent(mask, 0x0a));
    assert(mlxP1SetEvent(mask, 0x0b));
    assert(mlxP1SetEvent(mask, 0x40));
    assert(mask[0] == ((1ULL << 0) | (1ULL << 10) | (1ULL << 11)));
    assert(mask[1] == 1);
    assert(!mlxP1SetEvent(mask, 256));
}

int main()
{
    static_assert(MLX_P1_INIT_HCA_IN_BYTES == 32, "INIT_HCA size");
    static_assert(MLX_P1_QUERY_HCA_CAP_OUT_BYTES == 4112,
                  "QUERY_HCA_CAP output size");
    testOutbox();
    testCapabilities();
    testPages();
    testEventMasks();
    return 0;
}
