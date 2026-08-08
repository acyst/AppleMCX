# AppleMCX.kext — Implementation Status Overview

> Updated: 2026-08-08
> Project: Mellanox mlx5 family NIC driver for macOS, providing RoCEv2 (RDMA over Converged Ethernet)
> Location: `mlx-kext/`

---

## 1. Project Overview

A KEXT-based driver for Mellanox ConnectX NICs on macOS (Apple Silicon), providing **RoCEv2** services. Built on a generic framework covering the full **ConnectX-4 ~ ConnectX-8 + BlueField-3/4** mlx5 family, with ConnectX-5 as the first implementation.

**Core architecture**: the Linux dual-module design (`mlx5_core` + `mlx5_ib`) maps to a single KEXT with multiple IOServices (core + verbs + ethernet layers), emulating the auxiliary bus hierarchy via published nub properties.

---

## 2. Code Size

| Part | Files | Lines |
|------|-------|-------|
| Kernel driver (Sources) | 20 cpp + 19 hpp | ~6,624 |
| Userspace (usermode) | 20 | ~3,464 |
| **Total** | 59 | ~10,088 |

### Kernel subsystem breakdown

| Subsystem | Lines | Files |
|-----------|-------|-------|
| Core (core) | ~2,135 | MlxCmd/EQ/UAR/Health/DMA/PCIDriver/Main |
| Hardware (hw) | ~982 | MlxRegs/WQE/Doorbell/HCA/CX4-7 |
| verbs layer (ib) | ~2,056 | RoCE/QP/CQ/MR/AH/GID/CC |
| Ethernet (netif) | ~611 | MlxEthernet |
| User interface (userclient) | ~955 | MlxUserClient + MlxUCIO |

---

## 3. Kernel Driver Status

### 3.1 Core layer (core)

| Class | Responsibility | Status | Ported from |
|-------|----------------|--------|-------------|
| `MlxCmd` | Command interface: descriptor queue + mailbox + XOR signature + doorbell | ✅ | cmd.c |
| `MlxEQ` | Event queues: CREATE_EQ, EQE owner bit, MSI-X interrupts | ✅ | eq.c |
| `MlxUAR` | User Access Region: UAR/BF register allocation + mapping | ✅ | uar.c |
| `MlxHealth` | Firmware health polling | ✅ | health.c |
| `MlxDMA` | DMA utilities: pin user memory → physical address + precise lookup | ✅ | mr.c (umem) |
| `MlxPCIDriver` | PCI binding + firmware handshake + model dispatch + nub publishing | ✅ | main.c |

### 3.2 Hardware layer (hw)

| File | Content |
|------|---------|
| `MlxRegs.hpp` | init_seg, doorbell offsets, command opcodes, RoCE ports, event types |
| `MlxWQE.hpp` | WQE/CQE/AV structures (static_assert verified) |
| `MlxDoorbell.hpp` | 64-bit doorbell write, DB record |
| `MlxHCA.hpp` | HCA abstract interface + factory dispatch declarations |
| `MlxHCAConnectX4-7.cpp` | Per-model implementations + sub-factories |

### 3.3 verbs layer (ib)

| Class | Responsibility | Status |
|-------|----------------|--------|
| `MlxRoCE` | verbs entry + staged init + event subscription | ✅ |
| `MlxQP` | QP create/state machine/modifyQP path encoding | ✅ |
| `MlxCQ` | CQ create + CQE buffer DMA | ✅ |
| `MlxMR` | Memory registration (CREATE_MKEY + PBL) | ✅ |
| `MlxAH` | Address handle AV encoding (RoCE + IB addressing) | ✅ |
| `MlxGID` | GID table management (SET_ROCE_ADDRESS) | ✅ |
| `MlxCC` | DCQCN congestion control parameters | ✅ |

### 3.4 Ethernet layer (netif)

| Class | Responsibility | Status |
|-------|----------------|--------|
| `MlxEthernet` | IOEthernetController attachment + TX/RX framework | ✅ |
| `MlxEthernetDriver` | Binding layer (match mlx_eth nub) | ✅ |
| `MlxEthRing` | WQ ring buffer | ✅ |

### 3.5 User interface (userclient)

`MlxUserClient`: **23 methods**, four categories:
- **verbs control**: QueryDevice/QueryPort/CreateQP/ModifyQP/DestroyQP/CreateCQ/DestroyCQ/RegMR/DeregMR/CreateAH/DestroyAH/GetGidIndex
- **firmware management**: AccessReg/FwCmd/QueryPages/PortStats/FwReset/QueryFwVer/QueryHealth
- **DMA/data path**: VirtToPhys/QueryCqCompletions
- **memory mapping**: clientMemoryForType (UAR/DB/CQE)

### 3.6 Model support

| Model | PCI ID | Status |
|-------|--------|--------|
| ConnectX-4/4LX | 0x1013/0x1015 | ✅ |
| ConnectX-5/5Ex | 0x1017/0x1019 | ✅ |
| ConnectX-6/6Dx/6LX | 0x101B/0x101D/0x101F | ✅ |
| ConnectX-7/8 | 0x1021/0x1023 | ✅ |
| BlueField-3/4 | 0xa2dc/0xa2df | ✅ |

### 3.7 Async events

- **EQ layer**: async_eq mask 0xFFFFFFFF (accept all); dispatch to notifier list
- **MlxRoCE**: subscribes to 8 event types (WQ_CATAS_ERROR/PATH_MIG/COMM_EST/COMPLETION/
  DEVICE_FATAL/PORT_STATE_CHANGE/GID_CHANGE/CLIENT_REREGISTER),
  records via ring buffer `queueAsyncEvent`, dequeues via `getAsyncEvent`
- **Userspace**: libmlx `mlx_get_async_event` → verbs `ibv_get_async_event`
  (non-blocking, EAGAIN when empty) → elementType maps device/CQ/QP/port
- Event constants: PORT_STATE_CHANGE=0x09, GID_CHANGE=0x08,
  DEVICE_FATAL=0x22, CLIENT_REREGISTER=0x30 (See Linux device.h:354)

### 3.8 Real device info

- `MlxUserClient::QueryFwVer` reads real deviceId from `MlxVendorInfo`,
  portType/numPorts from `MlxHcaCaps` (no hardcoded values)

### 3.9 Multi-device

- Per-instance index/name (`mlx5_0`, `mlx5_1`, ...) published as the `deviceName` property
- Userspace enumerates devices via `mlx_list_devices` + the deviceName property

---

## 4. Userspace Status

### 4.1 Libraries

| Library | Responsibility | Status |
|---------|----------------|--------|
| `libmlx` | Zero-copy data path (post_send direct doorbell + poll_cq) + DMA translation | ✅ |
| `libverbs` | Linux verbs API compatibility layer (verbs.h + verbs_compat.c) | ✅ |
| `libmft` | Firmware management (fw_ver/reg/write/fw_cmd/port_stats/health) | ✅ |

### 4.2 Tools

| Tool | Linux equivalent | Status |
|------|------------------|--------|
| `ibv_devinfo` | ibv_devinfo | ✅ |
| `ib_write_bw` | perftest | ✅ |
| `ib_send_bw` | perftest | ✅ |
| `ib_read_bw` | perftest | ✅ |
| `ib_write_lat` | perftest | ✅ |
| `ib_send_lat` | perftest | ✅ |
| `ib_read_lat` | perftest | ✅ |
| `mlxconfig` | mft/mlxconfig | ✅ |
| `mlxstatus` | mft/mlxstatus | ✅ |
| `mlxlink` | mft/mlxlink | ✅ |
| `mlxreg` | mft/mlxreg | ✅ |
| `mlnx_qos` | mlnx-tools/mlnx_qos | ✅ |

perftest tools use a real TCP handshake (`PT_PORT=18515`), exchanging QPN/RKEY/PSN/GID;
the QP state machine RST→INIT→RTR→RTS uses peer parameters for RoCE path encoding.

---

## 5. DMA Data Path

```
TX:  user data → VirtToPhys → WQE data_seg.addr (physical) → doorbell → hardware DMA
RX:  CQE buffer (kernel DMA-coherent) → CQC PAS → hardware writes
RQ:  post_recv buffer → VirtToPhys → RQ WQE address
UAR: userspace mmap (clientMemoryForType) → direct doorbell write
CMD: command queue page (withPhysicalMask)
EQ:  EQE ring buffer (withPhysicalMask)
```

`MlxDMA::lookupPhys` accumulates precisely per physical segment (including per-segment lengths).

---

## 6. Verification Status

- **20 kernel files + userspace files** pass the stub-environment syntax/type check
- The stub environment (`build/stub/`) simulates kernel APIs, enabling compile-time
  validation of hardware structures and API signatures without an SDK
- **Not done**: real macOS SDK compilation, hardware testing (requires Mac Pro + developer mode)

---

## 7. Known Limitations

| Item | Description |
|------|-------------|
| Real compilation verification | Requires macOS SDK + real hardware |
| RX path testing | post_recv wired to DMA; full RQ driver awaits real hardware |
| Multi-device testing | Code complete, awaits real hardware validation |
| SEND peer post_recv | Handshake complete; peer-side post_recv awaits hardware closure |

---

## 8. Build & Usage

```sh
# Build KEXT
make                              # kernel driver
make ARCH=x86_64                  # Intel compatible
make tools                        # userspace toolchain

# Build toolchain separately
cd usermode/toolchain && make

# Load (Apple Silicon requires developer mode)
make sign CODE_SIGN_ID="..."; sudo make deploy; sudo make load

# Verify
make status
```

---

## 9. Document Index

| Document | Content |
|----------|---------|
| `README.md` | English quickstart + model matrix + build |
| `README_zh.md` | Chinese quickstart + model matrix + build |
| `IMPLEMENTATION_STATUS.md` | This file: implementation status overview (English) |
| `LICENSE` | GPL-2.0 (ported from Linux mlx5_core) |
