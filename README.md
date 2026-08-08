# AppleMCX.kext — Generic Mellanox mlx5 Family Adapter Driver

A macOS (Apple Silicon) driver for Mellanox ConnectX series NICs, providing **RoCEv2** (RDMA over Converged Ethernet) services.

**Generic framework**: covers the entire ConnectX family (ConnectX-4 ~ ConnectX-8 + BlueField-3/4), with ConnectX-5 (mcx5) as the first implementation. All hardware structures faithfully mirror Linux `mlx5_core`; classes are uniformly prefixed with `Mlx`.

## Features

- **Full RoCEv2**: RC QP state machine, CQ completion events, zero-copy MR/DMA, AH/GID addressing, DCQCN congestion control
- **Full family support**: CX4-8 + BlueField, model factory dispatch chain; new models only need a PCI ID
- **Multi-device**: `deviceName` property + `mlx_list_devices` + verbs multi-device enumeration
- **IB link-layer reserve**: MlxPortType/IB addressing/AH IB branch
- **Async event loop closure**: EQ events → MlxUserClient → libmlx → `ibv_get_async_event`
- **perftest real handshake**: TCP exchange of QPN/RKEY/PSN/GID before the data loop

## Architecture

```
IOPCIDevice (ConnectX-5)
  → MlxPCIDriver (core: command interface / event queues / UAR / firmware init)
      → publishes nubs (mlx_rdma, mlx_eth)
          → MlxRoCE (verbs protocol layer: QP/CQ/MR/AH/GID/DCQCN)
          → MlxEthernetDriver (ethernet interface, skipped for IB ports)
  → IOServiceOpen → MlxUserClient → userspace libmlx
```

## Key Design

- **Single KEXT, multiple IOServices**: emulates the Linux auxiliary bus hierarchy, avoiding cross-KEXT dependencies
- **Hardware decoupling**: all submodules access hardware through the `MlxHCA` abstraction; adding a model requires only a PCI ID + capability deltas
- **Zero-copy data path**: userspace mmaps UAR to write doorbells directly and reads CQEs directly — no syscalls for RDMA
- **DCQCN in pure-firmware loop**: the driver only wraps the `MODIFY_CONG_PARAMS` command

## Directory Layout

```
mlx-kext/
├── LICENSE                  GPL-2.0
├── README.md                This file (English)
├── README_zh.md             Chinese README
├── IMPLEMENTATION_STATUS.md Implementation status overview (English)
├── Makefile                 Build entry (KEXT + toolchain)
├── AppleMCX.kext/            KEXT bundle (Info.plist + MacOS/AppleMCX)
├── Sources/
│   ├── hw/                  Hardware structures (MlxRegs/WQE/Doorbell/HCA + CX4-7)
│   ├── core/                Core layer (MlxPCIDriver/Cmd/EQ/UAR/Health/DMA)
│   ├── ib/                  verbs layer (RoCE/QP/CQ/MR/AH/GID/CC)
│   ├── netif/               Ethernet interface (MlxEthernet)
│   └── userclient/          IOUserClient + MlxUCIO.h (shared userspace header)
├── usermode/
│   ├── libmlx/              Zero-copy userspace library
│   └── toolchain/           Toolchain (libverbs/libmft + tools)
├── tests/                   Test tools (ibv_devinfo)
├── Tools/                   Build/sign/load scripts
└── build/stub/              Kernel API stubs (syntax check without SDK)
```

## Building

Prerequisites: macOS 13+, Xcode Command Line Tools.

```sh
make                        # build KEXT (auto-detects arm64e/x86_64)
make tools                  # build userspace toolchain
cd usermode/toolchain && make   # or build tools separately
make sign CODE_SIGN_ID="Apple Development: xxx@yyy.com"   # sign
sudo make deploy            # deploy to /Library/Extensions
sudo make load              # load
make status                 # verify
```

Or one-shot scripts:

```sh
Tools/build_kext.sh
sudo Tools/load_kext.sh
```

### Compile-time verification without macOS SDK

`build/stub/` simulates the kernel API (IOKit/libkern), enabling syntax/type checks without an SDK:

```sh
clang++ -std=c++17 -fno-exceptions -Ibuild/stub -ISources -ISources/hw \
  -ISources/core -ISources/ib -ISources/netif -ISources/userclient \
  -fsyntax-only <file>.cpp
```

## Apple Silicon Developer Mode (Required to Load KEXT)

KEXTs are deprecated by Apple; loading on Apple Silicon requires:

```sh
# 1. Disable security policy in developer mode
sudo nvram boot-args="amfi_get_out_of_my_way=1"   # development only!

# 2. Verify after reboot
csrutil status

# 3. Sign with a development certificate + entitlements (Tools/kext.entitlements)
```

> ⚠️ This project is for research/lab validation. Production distribution must consider Apple's KEXT restrictions.

## PCI ID Matching

All-family mlx5 adapters are enabled by default: **ConnectX-4/5/6/7/8** and **BlueField-3/4**.

```
0x101315b3 0x101715b3 0x101b15b3 0x101d15b3 0x101f15b3
0x102115b3 0x102315b3 0xa2dc15b3 0xa2df15b3
```

To enable other models, run `Tools/gen_pci_match.sh` to generate the full IOPCIMatch string and edit Info.plist.

### Supported Models

| Model | PCI ID | Status |
|-------|--------|--------|
| ConnectX-4 | 0x1013 (PF), 0x1014 (VF) | ✅ Adapter ready |
| ConnectX-4LX | 0x1015 (PF), 0x1016 (VF) | ✅ Adapter ready |
| ConnectX-5 | 0x1017 (PF), 0x1018 (VF) | ✅ Adapter ready |
| ConnectX-5Ex | 0x1019 (PF), 0x101A (VF) | ✅ Adapter ready |
| ConnectX-6 | 0x101B (PF), 0x101C (VF) | ✅ Adapter ready |
| ConnectX-6 Dx | 0x101D (PF), 0x101E (VF) | ✅ Adapter ready |
| ConnectX-6 LX | 0x101F | ✅ Adapter ready |
| ConnectX-7 | 0x1021 (PF), 0x1022 (VF) | ✅ Adapter ready |
| ConnectX-8 | 0x1023 | ✅ Adapter ready |
| BlueField-3 (CX7) | 0xa2dc | ✅ Adapter ready |
| BlueField-4 (CX8) | 0xa2df | ✅ Adapter ready |

> The driver code is common across all ConnectX models (consistent with Linux mlx5_core);
> differences are limited to PCI IDs, capability registers, and ISSI negotiation
> (older ConnectX-4 firmware may only support ISSI=0; the driver falls back automatically).

## Test Tools

```sh
cd usermode/toolchain && make

./ibv_devinfo    # query RDMA device capabilities
./ib_write_bw    # RDMA Write bandwidth (server/client, TCP :18515 handshake)
./ib_send_bw     # SEND bandwidth
./ib_read_bw     # RDMA Read bandwidth
./ib_write_lat   # RDMA Write latency
./mlxconfig      # firmware config query/set
./mlxstatus      # firmware status
./mlxlink        # link status
./mlxreg         # register read/write
./mlnx_qos       # RoCE QoS (PFC / priority mapping)
```

perftest tools exchange QPN/RKEY/PSN/GID over TCP `:18515` and complete the QP state
machine (RST→INIT→RTR→RTS); RDMA Write/Read use the peer RKEY.

## DMA Data Path

```
TX: user data → VirtToPhys → WQE data_seg.addr (physical) → doorbell → hardware DMA
RX: CQE buffer (kernel DMA-coherent) → CQC PAS → hardware writes
UAR: userspace mmap (clientMemoryForType) → direct doorbell write
CMD: command queue page (withPhysicalMask)
EQ:  EQE ring buffer (withPhysicalMask)
```

- The kernel-side `MlxDMA` pins user memory to obtain physical addresses (`MlxDMAReq` carries per-segment lengths for precise translation)
- QP creation writes SQ/RQ buffer physical addresses into QPC wq_umem PAS
- CQ creation uses DMA-coherent memory for the CQE buffer

## Performance Tuning

DCQCN congestion-control parameters are configured via `IOConnectCallMethod(kMlxUCMethodCCModify)`,
defaults: `rpg_min_dec_fac=256, rpg_ai_rate=5, rpg_time_reset=55, rpg_threshold=150`.
The data path is zero-copy: userspace writes UAR doorbells directly and reads CQEs directly, with no syscalls.

## Status

| Capability | Status |
|------------|--------|
| Command interface / device init / firmware handshake / EQ / UAR / health | ✅ |
| Ethernet interface (TX/RX) | ✅ |
| RoCEv2 + RC data path (post_send/poll_cq) | ✅ |
| Async events: EQ → userspace `ibv_get_async_event` | ✅ |
| Real perftest TCP handshake (QPN/RKEY/PSN/GID) | ✅ |
| Multi-device (deviceName / mlx_list_devices) | ✅ |
| IB link-layer reserve (IB addressing / AH IB branch) | ✅ |

> Full implementation details: `IMPLEMENTATION_STATUS.md`; Chinese quickstart: `README_zh.md`.

## References

- Implementation status: `IMPLEMENTATION_STATUS.md`
- Chinese quickstart: `README_zh.md`
- Linux kernel `mlx5_core` driver source (kernel source tree): https://github.com/torvalds/linux (drivers/net/ethernet/mellanox/mlx5_core, drivers/infiniband/hw/mlx5)
- NVIDIA MLNX_OFED (driver bundle, `mlnx-ofed-kernel`): https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/ — file `MLNX_OFED_LINUX-5.9-0.5.6.0-ubuntu22.04-aarch64.tgz`

## License

This project is ported from the Linux kernel `mlx5_core` driver and is licensed under **GPL-2.0**. See `LICENSE`.
