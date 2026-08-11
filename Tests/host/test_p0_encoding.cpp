#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MlxP0Encoding.hpp"
#include "MlxWQE.hpp"

static void testWqeFlags()
{
    static_assert(sizeof(MlxWqeCtrlSeg) == 16, "WQE control size");
    static_assert(offsetof(MlxWqeCtrlSeg, fm_ce_se) == 11,
                  "fm_ce_se byte offset");
    static_assert(MLX_WQE_CTRL_CQ_UPDATE == 0x08, "CQ update encoding");
    static_assert(MLX_WQE_CTRL_SOLICIT == 0x02, "solicited encoding");

    MlxWqeCtrlSeg ctrl = {};
    ctrl.fm_ce_se = MLX_WQE_CTRL_CQ_UPDATE;
    assert(reinterpret_cast<uint8_t *>(&ctrl)[11] == 0x08);
}

static void assertCommandTail(const uint8_t *command)
{
    for (size_t i = MLX_QPC_BIT_OFFSET / 8 + MLX_QPC_BYTES;
         i < MLX_QP_MODIFY_IN_BYTES; i++)
        assert(command[i] == 0xa5);
}

static MlxRocePathFields makePath()
{
    MlxRocePathFields path = {};
    const uint8_t dmac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    const uint8_t dgid[16] = {
        0xfe, 0x80, 0, 0, 0, 0, 0, 0,
        0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55
    };
    memcpy(path.dmac, dmac, sizeof(dmac));
    memcpy(path.dgid, dgid, sizeof(dgid));
    path.sgidIndex = 7;
    path.hopLimit = 64;
    path.trafficClass = 0xab;
    path.udpSport = 0xc123;
    path.pkeyIndex = 0x1234;
    path.portNum = 1;
    return path;
}

static void testQpTransitions()
{
    uint8_t command[MLX_QP_MODIFY_IN_BYTES];
    memset(command, 0xa5, sizeof(command));
    uint8_t *qpc = command + MLX_QPC_BIT_OFFSET / 8;
    memset(qpc, 0, MLX_QPC_BYTES);

    uint32_t optParamMask = 0xffffffff;
    assert(mlxEncodeRst2InitQpc(qpc, MLX_QPC_BYTES, 0x1234, 1,
                                &optParamMask));
    assert(optParamMask == 0);
    assert(mlxGetBits(qpc, 0x08, 8) == 0);
    assert(mlxGetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x10, 16) ==
           0x1234);
    assert(mlxGetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x128, 8) == 1);
    assertCommandTail(command);

    memset(qpc, 0, MLX_QPC_BYTES);
    MlxRocePathFields path = makePath();
    assert(mlxEncodeInit2RtrQpc(qpc, MLX_QPC_BYTES, &path, 0x345678, 5,
                                0x234567, 16, &optParamMask));
    assert(optParamMask == ((1u << 1) | (1u << 3)));
    assert(mlxGetBits(qpc, 0x40, 3) == 5);
    assert(mlxGetBits(qpc, 0xa8, 24) == 0x345678);
    assert(mlxGetBits(qpc, 0x4a8, 24) == 0x234567);
    assert(mlxGetBits(qpc, 0x488, 3) == 4);
    assert(mlxGetBits(qpc, 0x490, 1) == 1);
    assert(mlxGetBits(qpc, 0x491, 1) == 1);

    const uint32_t ads = MLX_QPC_PRIMARY_PATH_BIT_OFFSET;
    assert(mlxGetBits(qpc, ads + 0x10, 16) == 0x1234);
    assert(mlxGetBits(qpc, ads + 0x48, 8) == 7);
    assert(mlxGetBits(qpc, ads + 0x58, 8) == 64);
    assert(mlxGetBits(qpc, ads + 0x108, 2) == 0);
    assert(mlxGetBits(qpc, ads + 0x10a, 6) == (0xab >> 2));
    assert(mlxGetBits(qpc, ads + 0x110, 16) == 0xc123);
    assert(mlxGetBits(qpc, ads + 0x128, 8) == 1);
    assert(memcmp(qpc + (ads + 0x80) / 8, path.dgid,
                  sizeof(path.dgid)) == 0);
    assert(memcmp(qpc + (ads + 0x130) / 8, path.dmac,
                  sizeof(path.dmac)) == 0);

    assertCommandTail(command);

    memset(qpc, 0, MLX_QPC_BYTES);
    assert(mlxEncodeRtr2RtsQpc(qpc, MLX_QPC_BYTES, 0x456789, 12, 8,
                               &optParamMask));
    assert(optParamMask == (1u << 6));
    assert(mlxGetBits(qpc, 0x3c8, 24) == 0x456789);
    assert(mlxGetBits(qpc, 0x4a3, 5) == 12);
    assert(mlxGetBits(qpc, 0x388, 3) == 3);
    assertCommandTail(command);
}

static void testMkey(uint32_t pageCount)
{
    uint64_t pages[480];
    for (uint32_t i = 0; i < pageCount; i++)
        pages[i] = 0x100000 + static_cast<uint64_t>(i) * 4096;

    uint8_t command[4112] = {};
    uint32_t inputSize = 0;
    assert(mlxEncodeCreateMkey(
        command, sizeof(command), pages, pageCount, 0x12345000,
        static_cast<uint64_t>(pageCount) * 4096,
        MLX_MR_ACCESS_LOCAL_WRITE | MLX_MR_ACCESS_REMOTE_WRITE |
            MLX_MR_ACCESS_REMOTE_READ,
        0x123456, 0x5a, &inputSize));
    assert(inputSize == mlxCreateMkeyInputSize(pageCount));

    uint8_t *mkc = command + MLX_CREATE_MKEY_MKC_BIT_OFFSET / 8;
    assert(mlxGetBits(mkc, 0x10, 1) == 1);
    assert(mlxGetBits(mkc, 0x11, 1) == 0);
    assert(mlxGetBits(mkc, 0x12, 1) == 1);
    assert(mlxGetBits(mkc, 0x13, 1) == 1);
    assert(mlxGetBits(mkc, 0x14, 1) == 1);
    assert(mlxGetBits(mkc, 0x15, 1) == 1);
    assert(mlxGetBits(mkc, 0x16, 2) == 1);
    assert(mlxGetBits(mkc, 0x20, 24) == 0xffffff);
    assert(mlxGetBits(mkc, 0x38, 8) == 0x5a);
    assert(mlxGetBits(mkc, 0x68, 24) == 0x123456);
    assert(mlxGetBits(mkc, 0x80, 64) == 0x12345000);
    assert(mlxGetBits(mkc, 0xc0, 64) ==
           static_cast<uint64_t>(pageCount) * 4096);
    assert(mlxGetBits(mkc, 0x1a0, 32) == mlxMttOctwordCount(pageCount));
    assert(mlxGetBits(mkc, 0x1db, 5) == 12);
    assert(mlxGetBits(command, MLX_CREATE_MKEY_ACTUAL_XLT_BIT_OFFSET, 32) ==
           mlxMttOctwordCount(pageCount));
    for (uint32_t i = 0; i < pageCount; i++)
        assert(mlxGetBits(command, 0x880 + i * 64, 64) == pages[i]);
    if (pageCount & 1)
        assert(mlxGetBits(command, 0x880 + pageCount * 64, 64) == 0);
}

static void testPageExpansion()
{
    uint64_t pages[8] = {};
    uint32_t count = 0;
    assert(mlxAppendMttPages(0x1003, 5000, pages, 8, &count));
    assert(count == 2 && pages[0] == 0x1000 && pages[1] == 0x2000);
    assert(mlxAppendMttPages(0x9000, 8192, pages, 8, &count));
    assert(count == 4 && pages[2] == 0x9000 && pages[3] == 0xa000);
    assert(mlxMttPageCount(0x1003, 5000) == 2);
    assert(mlxMttPageCount(UINT64_MAX - 1, 4) == 0);
}

static void testFragmentedMkey()
{
    const uint64_t pages[4] = { 0x1000, 0x9000, 0x30000, 0x41000 };
    uint8_t command[4112] = {};
    uint32_t inputSize = 0;
    assert(mlxEncodeCreateMkey(command, sizeof(command), pages, 4,
                              0x200000, 4 * 4096, 0, 1, 1, &inputSize));
    assert(inputSize == MLX_CREATE_MKEY_FIXED_BYTES + 32);
    for (uint32_t i = 0; i < 4; i++)
        assert(mlxGetBits(command, 0x880 + i * 64, 64) == pages[i]);
}

int main()
{
    testWqeFlags();
    testQpTransitions();
    testMkey(1);       /* 4 KiB */
    testMkey(16);      /* 64 KiB */
    testMkey(256);     /* 1 MiB */
    testPageExpansion();
    testFragmentedMkey();
    assert(mlxComposeMkey(0x123456, 0x5a) == 0x1234565a);
    assert(mlxMkeyIndex(0x1234565a) == 0x123456);
    uint64_t page = 0x1000;
    uint8_t command[4112] = {};
    assert(!mlxEncodeCreateMkey(command, sizeof(command), &page,
                               MLX_CREATE_MKEY_MAX_PAGES + 1, 0x1000, 4096,
                               0, 1, 1, NULL));
    return 0;
}
