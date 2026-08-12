# AppleMCX.kext Implementation Status

> Updated: 2026-08-12
>
> Classification: research prototype, not ready for hardware loading or
> production use.

## Verification Labels

| Label | Current state |
|-------|---------------|
| Code implemented | Phase 0 and Phase 1 paths are implemented |
| Host-tested | Yes: `make check-host`, ASan/UBSan encoders, policy gates |
| macOS 26 SDK build verified | Previous baseline only; current P1 changes await CI |
| Hardware verified | No |
| Traffic verified | No |
| Model certified | None |

## Core Status

| Area | Implementation state | Verification state |
|------|----------------------|--------------------|
| Firmware commands | Delivery plus outbox status/syndrome checking; timeout quarantine | Host-tested encoding/policy only |
| Firmware pages | Boot/init/runtime GIVE/TAKE, page worker, ownership tracking, reclaim | Host-tested encoding/policy only |
| Capabilities | Separate general, Ethernet, RoCE, and NIC flow-table queries | Constructed capability pages only |
| EQ and interrupts | 256-bit masks, callback synchronization, spare-budget CI updates, initial arm, two-vector MSI-X request | No interrupt delivery test |
| UAR | Negotiated system-page geometry and 4 KiB UAR offset | No MMIO test |
| Health | Real timer/counter, missed-update threshold, syndrome/RFR parsing | No firmware health test |
| DMA teardown | Graceful teardown releases mappings; ambiguous teardown intentionally retains them | No reset or IOMMU drain test |

## Data Paths

| Path | State |
|------|-------|
| Ethernet | Single SQ/RQ implementation exists; publication requires verified Ethernet and flow-table capabilities; no packets tested |
| RoCE | Control and encoding code exists, but `mlx_rdma` is disabled by default until the userspace UAR/DB isolation ABI is redesigned |
| Userspace tools | Buildable command-line/API prototypes; successful traffic is not claimed |

The existing userspace direct-UAR and shared DB mapping design is not considered
safe for publication. Compile-time opt-in does not make it production-safe.

## Hardware Scope

PCI IDs are recognized for multiple mlx5 generations, but recognition is not
hardware support certification. The first controlled target is one ConnectX-5
PF and one Ethernet port. ConnectX-4/6/7/8 and BlueField entries are unverified.

## Known Blockers

- Current P1 changes must pass the macOS 26/arm64e CI build.
- KEXT load/unload, PCI binding, MSI-X delivery, EQ/CQ routing, firmware command
  acceptance, UAR doorbells, DMA/IOMMU behavior, and Ethernet traffic remain
  unverified.
- No trusted reset/IOMMU-quiesce API is implemented for abnormal teardown.
  Ambiguous DMA mappings are deliberately retained.
- RoCE userspace isolation, hardware PD allocation, and per-client doorbells
  remain incomplete, so RoCE publication stays off.
- Sleep/wake, hot removal, reset, repeated lifecycle, and multi-device behavior
  have not been tested.

## Commands

```sh
make check-host
make ARCH=arm64e       # macOS SDK environment only
make tools
```

Do not run `make load` on physical hardware until the Phase 1 gate in
`REMEDIATION_PLAN.md` is explicitly opened after CI review.
