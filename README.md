# AppleMCX.kext — Generic Mellanox mlx5 Family Adapter Driver

A research-stage macOS mlx5 driver. ConnectX-5 PF Ethernet bring-up is the first
controlled hardware target. RoCE code is present but is disabled by default.

The framework recognizes PCI IDs across ConnectX-4 through ConnectX-8 and
BlueField, but no model is hardware-certified. Command layouts are based on
MLNX OFED 5.9.

## Features

- **Phase 1 core implementation**: firmware pages, command outbox checking, capability queries, EQ/MSI-X setup, UAR geometry, and health timer
- **Fail-closed publication**: Ethernet requires queried Ethernet/flow capabilities; RoCE remains off by default
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
- **Userspace data path prototype**: direct UAR/DB mappings are not published by default because per-client isolation is incomplete
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
│   ├── libmlx/              Userspace data-path prototype library
│   └── toolchain/           Toolchain (libverbs/libmft + tools)
├── Tests/host/              Host encoder and policy tests
├── Tools/                   Build/sign/load scripts
└── REMEDIATION_PLAN.md      Safety gates and staged bring-up plan
```

## Building

Prerequisites: macOS 13+, Xcode Command Line Tools.

```sh
make                        # build KEXT (auto-detects arm64e/x86_64)
make tools                  # build userspace toolchain
cd usermode/toolchain && make   # or build tools separately
make sign CODE_SIGN_ID="Apple Development: xxx@yyy.com"   # sign
sudo make deploy            # deploy to /Library/Extensions
sudo make load              # do not use before the Phase 1 hardware gate opens
make status                 # verify
```

Or one-shot scripts:

```sh
Tools/build_kext.sh
sudo Tools/load_kext.sh
```

### Host verification without macOS SDK

```sh
make check-host
```

This verifies pure IFC encoders and source safety policies. It does not compile
the KEXT against the macOS SDK and does not validate hardware behavior.

## Apple Silicon Developer Mode (Required to Load KEXT)

KEXTs are deprecated by Apple; loading on Apple Silicon requires:

```sh
# 1. Disable security policy in developer mode
sudo nvram boot-args="amfi_get_out_of_my_way=1"   # development only!

# 2. Verify after reboot
csrutil status

# 3. Sign with a development certificate + entitlements (Tools/kext.entitlements)
```

> This project is for research/lab validation. Do not load it on physical
> hardware until the Phase 1 gate in `REMEDIATION_PLAN.md` is opened.

## PCI ID Matching

The plist recognizes several mlx5 PCI IDs. Recognition does not mean that the
device has passed firmware, interrupt, DMA, lifecycle, or traffic validation.

```
0x101315b3 0x101715b3 0x101b15b3 0x101d15b3 0x101f15b3
0x102115b3 0x102315b3 0xa2dc15b3 0xa2df15b3
```

To enable other models, run `Tools/gen_pci_match.sh` to generate the full IOPCIMatch string and edit Info.plist.

### Supported Models

| Model | PCI ID | Status |
|-------|--------|--------|
| ConnectX-4/4LX | 0x1013/0x1015 | PCI ID recognized; unverified |
| ConnectX-5/5Ex | 0x1017/0x1019 | First PF target; not yet hardware verified |
| ConnectX-6/6Dx/6LX | 0x101B/0x101D/0x101F | PCI ID recognized; unverified |
| ConnectX-7/8 | 0x1021/0x1023 | PCI ID recognized; unverified |
| BlueField-3/4 | 0xa2dc/0xa2df | PCI ID recognized; unverified |

> No model support claim is made until it passes the validation matrix.

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

The tools contain a TCP parameter-exchange implementation. RDMA traffic has not
been executed or verified with this driver.

## DMA Data Path

```
TX: mapped user data → WQE data segment → doorbell → device DMA
RX: CQE buffer (kernel DMA-coherent) → CQC PAS → hardware writes
UAR: direct userspace mapping remains disabled by the default RoCE publication gate
CMD: command queue page (withPhysicalMask)
EQ:  EQE ring buffer (withPhysicalMask)
```

- The kernel-side `MlxDMA` prototype pins user memory and returns device-visible segments
- QP creation writes SQ/RQ buffer physical addresses into QPC wq_umem PAS
- CQ creation uses DMA-coherent memory for the CQE buffer

## Performance Tuning

DCQCN congestion-control parameters are configured via `IOConnectCallMethod(kMlxUCMethodCCModify)`,
defaults: `rpg_min_dec_fac=256, rpg_ai_rate=5, rpg_time_reset=55, rpg_threshold=150`.
These controls are not hardware-verified, and the direct userspace data path is
not enabled by default.

## Status

| Capability | Status |
|------------|--------|
| Phase 0/1 code implemented | Yes |
| Host tests | Passing |
| Current P1 macOS SDK build | Awaiting CI |
| Hardware and traffic | Not verified |
| RoCE publication | Disabled by default |
| Model certification | None |

> Full implementation details: `IMPLEMENTATION_STATUS.md`; Chinese quickstart: `README_zh.md`.

## References

- Implementation status: `IMPLEMENTATION_STATUS.md`
- Chinese quickstart: `README_zh.md`
- Linux kernel `mlx5_core` driver source (kernel source tree): https://github.com/torvalds/linux (drivers/net/ethernet/mellanox/mlx5_core, drivers/infiniband/hw/mlx5)
- NVIDIA MLNX_OFED (driver bundle, `mlnx-ofed-kernel`): https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/ — file `MLNX_OFED_LINUX-5.9-0.5.6.0-ubuntu22.04-aarch64.tgz`

## License

This project is ported from the Linux kernel `mlx5_core` driver and is licensed under **GPL-2.0**. See `LICENSE`.
