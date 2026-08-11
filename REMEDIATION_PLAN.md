# AppleMCX Remediation Plan

> Baseline: `ce35c32 Harden mlx5e Ethernet hardware data path`
>
> Updated: 2026-08-11
>
> Status: macOS 26/arm64e build verified; hardware operation not verified.

## 1. Objective

Bring AppleMCX from a buildable mlx5 research prototype to a driver that can be
safely loaded for controlled ConnectX-5 PF bring-up, then establish a reliable
single-queue Ethernet path before reopening the RoCE userspace data path.

The order is deliberate:

1. Eliminate kernel memory corruption and unsafe DMA lifetime paths.
2. Complete the mlx5 firmware initialization contract.
3. Validate one Ethernet TX/RX queue on one hardware target.
4. Design a safe per-client RoCE doorbell and memory-isolation ABI.
5. Expand functionality, performance, and hardware coverage only after the
   preceding gates pass.

## 2. Current Baseline

### Verified

- The KEXT and userspace toolchain build on the GitHub `macos-26` runner for
  `arm64e`.
- The command opcode table has been compared with MLNX OFED 5.9.
- Ethernet contains implementations for PD, TD, MKey, TIS, CQ, SQ, RQ, TIR,
  CQ polling, CQ arming, RX flow steering, TX bounce buffers, and RX buffers.
- Queue and packet DMA mappings are retained across their intended hardware
  object lifetime on the normal success path.
- CQE ownership initialization and separate SQ/RQ/CQ counters are present.
- Core cleanup disables PCI bus mastering when firmware teardown cannot be
  trusted as a DMA boundary.

### Not Verified

- KEXT load and unload on a real Mac.
- PCI binding to a physical ConnectX adapter.
- Firmware command acceptance and object context correctness.
- MSI-X delivery, EQ/CQ interrupt routing, and initial EQ arming.
- Apple Silicon PCIe DMA coherency and IOMMU address use by mlx5 MKeys.
- Ethernet packet transmission or reception.
- RoCE QP operation or interoperability.
- Sleep/wake, reset, hot removal, or repeated lifecycle behavior.

### Current Release Classification

- Core: prototype.
- Ethernet: single-queue bring-up implementation, not hardware validated.
- RoCE: control-plane prototype; userspace data path disabled.
- Tools: buildable API and CLI prototypes.
- Hardware support: PCI IDs recognized, no model certified.
- Production readiness: not ready.

## 3. Engineering Rules

- Use MLNX OFED 5.9 `mlx5_ifc.h` and associated mlx5 headers as the command and
  bitfield authority.
- Generate or wrap field access consistently. Do not encode IFC bit offsets as
  C byte offsets.
- Check both command delivery status and firmware outbox status/syndrome.
- Never release DMA memory until the referencing hardware object is confirmed
  destroyed or PCI bus mastering is disabled.
- Fail closed when capability, interrupt, object state, or DMA behavior cannot
  be established.
- Do not expose a device-global UAR or DB page to userspace.
- Keep the initial hardware target limited to one ConnectX-5 PF and one port.
- Do not claim support for a model until that model has passed the hardware
  validation matrix.
- Do not add performance features until the single-queue path is correct and
  recoverable.

## 4. Phase 0: Immediate Safety Fixes

Priority: P0

Goal: remove known kernel corruption risks and stop publishing unusable data
paths before any hardware loading attempt.

### 4.1 Fix QP Modify Buffer Corruption

Problem:

- `MlxQP::modifyQP()` allocates a 272-byte command buffer.
- `encodePath(ctx, qpc + 0xC0)` treats an IFC bit offset as a byte offset.
- `encodePath()` then writes through offset `0x80`, exceeding the stack buffer.

Files:

- `Sources/ib/MlxQP.cpp`

Work:

- Replace byte-offset path encoding with `mlxSetBits()` against the QPC base.
- Size modify-QP inputs from the complete IFC command layout.
- Encode the primary address path at its actual bit position.
- Add compile-time or host-side tests covering every written field and the
  final command size.

Acceptance:

- AddressSanitizer-enabled host test reports no overflow in all QP transitions.
- Every QP field written by the driver is checked against OFED 5.9 offsets.
- RST to INIT, INIT to RTR, and RTR to RTS commands remain within the declared
  input buffer.

### 4.2 Rewrite RoCE CREATE_MKEY Encoding

Problem:

- `MlxMR::cmdCreateMKey()` treats MKC bit offsets such as `0x120` and `0x1e0`
  as byte offsets.
- The current MKC and PAS placement does not match `create_mkey_in`.
- The returned lkey/rkey uses only the MKey index and lacks a key variant.

Files:

- `Sources/ib/MlxMR.cpp`
- `Sources/ib/MlxMR.hpp`

Work:

- Set MKC fields with bit helpers from the command base and MKC base.
- Place MKC at command bit `0x80` and PAS/MTT at command bit `0x880`.
- Set `translations_octword_size`, actual translation size, page size, PD,
  address, length, access mode, and permissions according to the IFC.
- Generate lkey/rkey as `(mkey_index << 8) | key_variant`.
- Define key-variant allocation and stale-key behavior.
- Validate PBL expansion page by page rather than assuming one returned DMA
  segment equals one page.

Acceptance:

- Host tests compare a generated command image with expected IFC bit values.
- A 4 KiB, 64 KiB, 1 MiB, and fragmented MR produce a valid page list without
  command-buffer overlap.
- Keys contain both index and variant portions.

### 4.3 Correct WQE Control Flags

Problem:

- `MLX_WQE_CTRL_CQ_UPDATE` is `0x02`; mlx5 requires `2 << 2`, or `0x08`.
- The same incorrect value is used by Ethernet and userspace RoCE TX.

Files:

- `Sources/hw/MlxWQE.hpp`
- `Sources/netif/MlxEthernet.cpp`
- `usermode/libmlx/libmlx.c`

Work:

- Define CQ update as `0x08` and solicited event as `0x02`.
- Audit all `fm_ce_se` writes.
- Add a host test for control-segment byte encoding.

Acceptance:

- Ethernet and RoCE WQEs use the corrected completion-request value.
- No literal `0x02` remains as a CQ update request.

### 4.4 Gate RoCE Publication

Problem:

- UAR and DB record mmap are intentionally rejected for security.
- `libmlx` requires both mappings and therefore cannot create a usable QP.
- Publishing the service currently implies a data path that cannot operate.

Files:

- `Sources/core/MlxPCIDriver.cpp`
- `Sources/ib/MlxRoCE.cpp`
- `AppleMCX.kext/Contents/Info.plist`

Work:

- Add an explicit development capability/property gate for `mlx_rdma`.
- Default the gate off until the Phase 3 ABI is implemented.
- Keep RoCE code buildable and testable without attaching it on hardware.

Acceptance:

- Default hardware bring-up publishes Ethernet only.
- RoCE cannot be enabled accidentally by a documentation-only setting.

### Phase 0 Gate

Do not load the KEXT on physical hardware until all Phase 0 items pass host
tests and macOS 26 CI.

## 5. Phase 1: Firmware and Core Initialization

Priority: P0

Goal: implement the required mlx5 initialization contract and make command and
event success trustworthy.

### 5.1 Check Firmware Outbox Status

Files:

- `Sources/core/MlxCmd.cpp`
- `Sources/core/MlxCmd.hpp`

Work:

- After copying the response, parse outbox `status` and `syndrome`.
- Map firmware status values to appropriate `IOReturn` values.
- Log opcode, op_mod, status, and syndrome.
- Treat a nonzero outbox status as command failure even when delivery status is
  successful.
- Preserve timeout quarantine behavior.

Acceptance:

- Injected nonzero outbox status is propagated to every caller.
- Object IDs are never parsed from failed commands.
- Destroy failures retain DMA resources or cross a verified bus-master-off
  boundary.

### 5.2 Implement Firmware Startup Pages

Files:

- Add a core page-management class or keep the implementation local to core if
  it remains small.
- `Sources/core/MlxPCIDriver.cpp`
- `Sources/core/MlxEQ.cpp`
- `Sources/hw/MlxRegs.hpp`

Work:

- Query and provide boot pages before the required initialization step.
- Query and provide init pages at the OFED-defined point.
- Retain every page descriptor and DMA mapping while firmware owns it.
- Parse PAGE_REQUEST EQEs and satisfy runtime requests.
- Reclaim startup and runtime pages during teardown.
- Reserve or explicitly manage the page-command command slot.

Acceptance:

- Startup command order matches OFED 5.9 `mlx5_function_setup()`.
- Every provided page is tracked by function ID and ownership state.
- Teardown either reclaims all pages or disables bus mastering before release.
- PAGE_REQUEST has an internal handler and cannot be silently consumed.

### 5.3 Correct Command Sizes

Files:

- `Sources/core/MlxPCIDriver.cpp`
- All typed command constructors under `Sources/`

Work:

- Use 32 bytes for `INIT_HCA` input.
- Use the full 4112-byte `QUERY_HCA_CAP` output.
- Audit every command against the IFC structure size, including Ethernet and
  RoCE commands.
- Add one centralized size table or host-generated assertions.

Acceptance:

- All command buffers fit within `MLX_CMD_MAX_SIZE`.
- Every command's input and output length matches the referenced IFC type.

### 5.4 Implement Capability Negotiation

Files:

- `Sources/core/MlxPCIDriver.cpp`
- `Sources/hw/MlxHCA.hpp`

Work:

- Query max and current general capabilities separately.
- Correct the RoCE, `uar_4k`, BF, flow-table, queue, and port field offsets.
- Convert encoded GID and PKey table sizes to software sizes.
- Implement the required `SET_HCA_CAP` changes.
- Query RoCE and NIC flow-table capability unions before enabling those paths.
- Reject unsupported combinations rather than filling synthetic capability
  values.

Acceptance:

- Parsed fields have host tests against captured or constructed capability
  pages.
- `mlx_eth` is published only when required Ethernet and flow capabilities are
  present.
- `mlx_rdma` remains off unless all required RoCE capabilities are verified.

### 5.5 Repair EQ Creation and Arming

Files:

- `Sources/core/MlxEQ.cpp`
- `Sources/core/MlxEQ.hpp`

Work:

- Change event masks to four 64-bit words.
- Serialize all four words with `OSWriteBigInt64`.
- Remove overlapping catch-all masks unless explicitly required.
- Add spare EQEs when calculating ring depth.
- Arm every EQ after its interrupt source is successfully attached.
- Fail or reduce queue count when a requested interrupt source is unavailable.
- Handle all set command-completion bits, not only the lowest bit.
- Reject or safely handle a zero command-completion vector.

Acceptance:

- Host tests verify CMD, PAGE_REQUEST, completion, port, and error event bits.
- Initial arm occurs before `start()` reports success.
- Missing completion vectors cannot be silently accepted.

### 5.6 Repair Event Semantics and Notifier Locking

Files:

- `Sources/hw/MlxRegs.hpp`
- `Sources/core/MlxEQ.cpp`
- `Sources/ib/MlxRoCE.cpp`

Work:

- Correct raw hardware event values such as WQ catastrophic error and NIC
  vport change.
- Separate raw mlx5 EQ event numbers from synthesized verbs event numbers.
- Expand event indexing to the hardware event range or validate all supported
  values explicitly.
- Copy or retain notifier targets under the registry lock, then invoke them
  after releasing the lock.
- Do not execute firmware commands, mbuf allocation, or full CQ polling while
  holding the notifier registry lock.

Acceptance:

- Notifier callbacks can register or unregister without deadlock.
- Port down is not interpreted as port active.
- Every supported raw event has an explicit translation policy.

### 5.7 Implement Real Health Monitoring

Files:

- `Sources/hw/MlxRegs.hpp`
- `Sources/core/MlxHealth.cpp`
- `Sources/core/MlxHealth.hpp`

Work:

- Correct the init-segment health buffer layout.
- Read the real `health_counter` after the firmware health buffer.
- Add an `IOTimerEventSource` or equivalent workloop timer.
- Require multiple missed updates before declaring a watchdog condition.
- Parse syndrome and extended syndrome.
- Initially fail closed and disable bus mastering on fatal health state; add
  reset/recovery only after teardown is proven safe.

Acceptance:

- Health state changes only after the configured threshold.
- Timer start/stop is synchronized with device lifecycle.
- `QueryHealth` reports measured state rather than an initial default.

### Phase 1 Gate

Hardware testing may begin only when firmware startup pages, outbox status,
capabilities, EQ masks, initial arm, and interrupt-source validation are all in
place.

## 6. Phase 2: ConnectX-5 Ethernet Bring-Up

Priority: P0/P1

Goal: validate a correct and recoverable one-SQ/one-RQ Ethernet path on a
ConnectX-5 PF before adding features or more models.

### 6.1 Restrict the Hardware Target

Files:

- `AppleMCX.kext/Contents/Info.plist`
- `Sources/hw/MlxHCAConnectX5.cpp`
- Documentation

Work:

- Default-match only the selected ConnectX-5 PF ID during bring-up.
- Keep other IDs documented as recognized but disabled and unverified.
- Record firmware version, PCI revision, port mode, and UAR geometry for every
  hardware test.

Acceptance:

- The test KEXT cannot bind to an unvalidated adapter generation by default.

### 6.2 Correct the IOKit Child Lifecycle

Files:

- `Sources/netif/MlxEthernet.cpp`

Work:

- Attach `MlxEthernet` to the provider before calling `start()`.
- On stop, call `stop()`, detach, and release in the correct order.
- Check the result of `attach()` and `registerService()` prerequisites.
- Check `IOLockAlloc()` and all partially initialized members.
- Replace the current quarantine return from `free()` with explicit ownership
  held by a core quarantine object or a bus-master-off final release path.

Acceptance:

- Failed start, normal stop, and provider termination have balanced
  retain/attach/start/stop/detach/release transitions.
- No path exits `free()` without a defined final owner.

### 6.3 Validate Object Creation by Readback

Work:

- After each create/modify during bring-up, query the object context where the
  command exists and compare critical fields.
- Validate MKey, CQ, SQ, RQ, TIS, TIR, and flow-table identifiers and states.
- Keep readback logging behind a development flag.

Acceptance:

- Every queue reaches the intended state before the root flow table is
  connected.
- Failed validation tears down in dependency order without releasing live DMA.

### 6.4 RX-Only Bring-Up

Work:

- Start with TX disabled.
- Verify RQ producer DB, CQ owner phase, CQN, WQE counter, byte count, and packet
  content.
- Verify whether `mbuf_allocpacket()` provides enough contiguous first-buffer
  space; otherwise copy through the mbuf chain.
- Add a bounded CQ poll budget and rescheduling mechanism.
- Add RX packet, byte, allocation-failure, invalid-CQE, and repost-failure
  counters.

Acceptance:

- ARP, IPv4, IPv6, ICMP, and sustained RX pass without CQ or RQ stalls.
- CQ wraps multiple times with correct owner handling.
- RX allocation failure does not corrupt or permanently lose RQ capacity.

### 6.5 TX Bring-Up

Work:

- Verify one WQE and one CQE before enabling normal output.
- Confirm SQ producer, SQ consumer, CQ producer, CQ consumer, and WQE counter.
- Decode request-error CQEs and log syndrome.
- Add queue stop/wake behavior based on actual outstanding WQEs.
- Define oversized packet behavior explicitly instead of silently consuming and
  dropping it.
- Verify BF/UAR mapping and ordering on Apple Silicon.

Acceptance:

- More than 100 full SQ wraps complete without a stall or mbuf leak.
- Error CQEs stop or rebuild the queue rather than being treated as success.
- Packet contents match the source under small, minimum-size, and MTU-size
  traffic.

### 6.6 Flow Steering and Link State

Work:

- Query NIC_RX flow-table and root-modification capabilities.
- Record the pre-existing root state where firmware supports it.
- Define a shared flow namespace instead of unconditionally replacing the
  global root.
- Add MAC unicast, broadcast, and required multicast rules.
- Keep promiscuous mode and multicast APIs unsupported until hardware rules are
  implemented; do not advertise unsupported packet filters.
- Query real link speed and publish medium information.

Acceptance:

- Link up/down and speed are reported accurately.
- Disabling Ethernet restores a valid default or previous steering state.
- Packets not intended for the local MAC follow the selected filter policy.

### 6.7 Ethernet Error Recovery

Work:

- Handle CQ error, WQ catastrophic error, request error, response error, CQ
  overrun, SQ error, and RQ error.
- Stop new TX before teardown.
- Add queue transition or rebuild logic for recoverable errors.
- Escalate unrecoverable errors to device-level shutdown and bus-master disable.

Acceptance:

- Injected queue errors do not result in DMA use-after-free.
- A recoverable queue error can restore traffic without reloading the KEXT.
- An unrecoverable error leaves the device quiesced and resources safely owned.

### 6.8 Ethernet Hardware Test Matrix

Required tests:

- Load, bind, interface attach, and unload.
- Link down/up and cable removal.
- DHCP or static IPv4 and IPv6.
- Bidirectional ping with minimum, normal, and MTU-sized packets.
- Bidirectional `iperf3`.
- TX and RX CQ wrap.
- Ring-full and mbuf-allocation pressure.
- 1,000 interface enable/disable cycles.
- Repeated KEXT load/unload where supported.
- Sleep/wake.
- Multi-hour bidirectional soak.

Acceptance:

- No panic, DMA fault, CQ stall, stale mbuf, or leaked firmware object.
- Software counters agree with observed traffic and available hardware counters.

### Phase 2 Gate

Do not enable RoCE on hardware until the Ethernet single-queue path passes the
full Phase 2 matrix and its flow-root behavior is understood.

## 7. Phase 3: Safe RoCE Userspace ABI

Priority: P1

Goal: make the userspace data path reachable without exposing device-global
doorbells or weakening process isolation.

### 7.1 Select the Doorbell Model

Choose one design before implementation.

Option A: per-client direct doorbells

- Allocate a dedicated UAR or safely partitioned UAR resource per client.
- Allocate a per-client DB page.
- Map only the client's UAR/BF range and DB page.
- Allocate a unique BF register for each posting context.
- Enforce task ownership and revoke mappings on client close.

Option B: kernel-mediated posting

- Keep UAR and DB pages unmapped.
- Add validated post-send, post-recv, and CQ-arm methods.
- Copy or map WQEs through controlled memory and ring the doorbell in kernel.
- Accept higher syscall overhead for the first safe implementation.

Initial recommendation: Option B for first hardware validation; move to Option
A only after isolation and mapping behavior are proven.

Acceptance:

- No client can write another client's DB record or BF register.
- Closing a client revokes all mappings and stops all associated DMA.

### 7.2 Implement Real PD Ownership

Work:

- Add UserClient `ALLOC_PD` and `DEALLOC_PD` methods.
- Track PD ownership per client.
- Require owned PDs for QP and MR creation.
- Reject cross-client QP, CQ, MR, AH, and PD handles.
- Release all owned resources in dependency order on client close.

Acceptance:

- Two clients receive distinct hardware PDs.
- Cross-client resource use is rejected by both software validation and the NIC
  protection domain.

### 7.3 Correct QP WQ Memory Geometry

Work:

- Define one valid mlx5 WQ memory layout with explicit SQ and RQ offsets.
- Expand IOMMU segments into the PAS page sequence required by the IFC.
- Set QPC UAR page, page size, SQ/RQ sizes, strides, CQ numbers, DB address,
  transport type, and PD correctly.
- Do not set UMEM-valid fields unless a real firmware UMEM object exists.
- Add per-QP BF allocation if direct doorbells are selected.

Acceptance:

- CREATE_QP succeeds and QUERY_QP returns the expected geometry.
- Posting and completing more than 100 SQ/RQ wraps does not overwrite live WQEs.

### 7.4 Complete QP State and Path Encoding

Work:

- Preserve and translate the verbs `attr_mask`.
- Encode access flags, timeout, retry count, RNR retry, RD atomic limits, PSNs,
  MTU, destination QPN, and address path.
- Implement ERR and RESET transitions before advanced states.
- Parse QP event handles from EQEs.
- Implement `QueryQP` for validation and diagnostics.

Acceptance:

- Invalid transitions fail without changing software state.
- Valid RC transitions read back with all requested attributes.
- QP error events identify the affected QPN.

### 7.5 Complete CQ Semantics

Work:

- Remove the `depth * 2` lifetime limit from consumer updates.
- Track monotonic consumer progress and validate it against known producer or
  allowed ring distance.
- Implement CQ arm and `ibv_req_notify_cq()`.
- Support multiple CQs per client and map them by explicit memory type/offset.
- Preserve WR IDs per SQ/RQ WQE and translate CQEs to standard `ibv_wc` fields.
- Implement a real blocking completion channel or document polling-only mode.

Acceptance:

- CQ can wrap indefinitely.
- `wr_id`, opcode, status, byte length, QPN, and vendor syndrome are correct.
- Multiple CQs receive independent events.

### 7.6 Implement Real GID and Neighbor Management

Work:

- Obtain the actual Ethernet MAC from the shared port state.
- Track macOS IPv4/IPv6 address changes and VLAN state.
- Program RoCEv2 GIDs with the correct source MAC, L3 type, and version.
- Resolve destination MAC addresses through a constrained userspace policy
  daemon or kernel networking integration.
- Generate a valid RoCEv2 UDP source port.
- Keep GID allocation bitmap and programmed table state synchronized.

Acceptance:

- `ibv_query_gid()` returns the actual programmed GID.
- Address removal removes the corresponding hardware GID.
- RoCE frames use the same physical MAC identity as Ethernet.

### 7.7 Defer UD and DCQCN Claims

Work:

- Keep UD unsupported until a complete datagram segment, AH, QKey, destination
  QPN, and receive path are implemented.
- Keep DCQCN query/modify experimental until capabilities, complete parameter
  layouts, and readback are implemented.

Acceptance:

- Public capability reporting never advertises UD or DCQCN before their tests
  pass.

### Phase 3 Gate

RoCE is publishable only after PD isolation, QP/MR/CQ correctness, a safe
doorbell design, real GID management, and client-close cleanup are verified.

## 8. Phase 4: RoCE End-to-End Validation

Priority: P1/P2

Goal: prove data correctness before performance measurement.

### 8.1 Repair Perftest Protocol

Files:

- `usermode/toolchain/perftest/perftest_common.h`
- `usermode/toolchain/perftest/ib_*`

Work:

- Exchange remote virtual address in addition to QPN, RKey, PSN, and GID.
- Use network byte order.
- Implement complete send/receive loops for partial TCP I/O.
- Post receive WRs before SEND traffic.
- Respect SQ/RQ depth and poll completions to reclaim entries.
- Check every `ibv_post_send()`, `ibv_post_recv()`, and `ibv_poll_cq()` result.
- Verify remote data contents or checksum.
- Measure bandwidth at completion, not at submission.
- Measure latency by completion or ping-pong, not by function-call duration.

Acceptance:

- Every iteration has a successful completion.
- Remote contents match expected data.
- Reported performance measures completed operations.

### 8.2 Two-Machine Validation

Work:

- Test Mac-to-Mac first.
- Test Mac-to-Linux mlx5 interoperability after the local path is stable.
- Cover RDMA Write, RDMA Read, and SEND/RECV separately.
- Test QP retry, RNR, remote access errors, and link interruption.

Acceptance:

- At least 1,000 successful operations per verb with data verification.
- Expected errors produce correct WC status without kernel instability.
- QP and CQ resources can be destroyed after success and failure.

## 9. Phase 5: Performance and Feature Expansion

Priority: P2

Begin only after correctness and recovery gates pass.

Ethernet candidates:

- Multiple TX/RX queues.
- RSS and RQT.
- Interrupt moderation and poll budgets.
- TX scatter/gather instead of bounce copies.
- RX page recycling.
- Checksum offload.
- VLAN insertion/filtering.
- TSO/LRO where supported by macOS KPI and hardware.
- Jumbo MTU.
- Hardware statistics and timestamps.

RoCE candidates:

- Direct per-client doorbells if Phase 3 initially uses mediated posting.
- Multiple QPs and CQs per process.
- Larger and fragmented MRs.
- Completion channels and asynchronous event delivery.
- UD support.
- DCQCN and QoS register integration.

Acceptance:

- Each feature has a capability gate, fallback behavior, host tests, and
  hardware regression coverage.

## 10. Phase 6: Hardware and Platform Expansion

Priority: P2/P3

Add one target at a time:

1. ConnectX-5Ex.
2. ConnectX-4/4LX, including ISSI 0 and UAR differences.
3. ConnectX-6/6Dx/6LX.
4. ConnectX-7/8.
5. BlueField-3/4 host functions.
6. VFs.
7. Multiple adapters.
8. Intel macOS where supported.

For each target, validate:

- PCI binding and function type.
- Firmware page requirements.
- Capability differences.
- UAR geometry.
- Available interrupt vectors.
- Ethernet lifecycle and traffic.
- RoCE lifecycle and traffic if enabled.
- Reset, sleep/wake, and teardown.

Do not mark a target supported solely because its PCI ID reaches the HCA
factory.

## 11. Test and CI Plan

### 11.1 Make Tests Reproducible

Problem:

- `build/` is ignored.
- `make clean` deletes `build/stub`.
- The existing local stub checks cannot be reproduced from a fresh clone.

Work:

- Move source stubs to a tracked location such as `Tests/stub/`.
- Add `make check`, `make check-kernel`, `make check-userspace`, and
  `make check-headers` targets.
- Keep generated test output under ignored `build/`.

### 11.2 Host-Side Unit Tests

Required coverage:

- `mlxSetBits()` and `mlxGetBits()` across byte boundaries.
- Command input/output sizes.
- Command outbox status mapping.
- EQ mask serialization.
- CQ/EQ owner phase and ring wrap.
- WQE control flags and endian layout.
- QP transition matrix and command bounds.
- MKey and PBL layout.
- GID and PKey size conversion.
- DB slot allocation and exhaustion.
- UserClient structure sizes and resource ownership.
- Perftest handshake framing and partial I/O.
- PCI ID consistency among Info.plist, HCA factory, generator, and docs.

### 11.3 CI Matrix

Add jobs for:

- macOS 26 arm64e KEXT build.
- x86_64 build on an available Intel runner or controlled builder.
- Userspace build with warnings as errors.
- Host/stub tests.
- Header self-containment.
- Info.plist lint and version consistency.
- Mach-O architecture and unresolved-symbol inspection.
- `codesign --verify --deep --strict` for signed artifacts.
- `pkgutil --check-signature` and package payload inspection.

CI success must continue to mean build/package success only. Hardware readiness
must be tracked separately.

### 11.4 Hardware CI or Lab Runs

Maintain machine-readable results for:

- Adapter and firmware identity.
- KEXT commit.
- macOS build.
- Load/bind result.
- Ethernet test matrix.
- RoCE test matrix.
- Panic, DMA fault, firmware syndrome, and resource leak observations.

## 12. Packaging and Documentation

### Packaging

- Distinguish ad-hoc development artifacts from signed release artifacts.
- Do not silently downgrade a release job to ad-hoc signing.
- Derive package architecture from the Mach-O binary, not `uname -m`.
- Sign installer packages when producing distributable artifacts.
- Validate installation, permissions, upgrade, and removal on a test machine.
- Use the target macOS loading workflow, including `kmutil` where required.

### Documentation

Replace binary completion marks with these states:

- Implemented in code.
- Host-tested.
- macOS SDK build verified.
- Hardware bring-up verified.
- Traffic verified.
- Model certified.

Update README and `IMPLEMENTATION_STATUS.md` after each phase gate. Do not use
"full", "ready", "zero syscall", or "supported" unless the corresponding
behavior has passed its acceptance tests.

## 13. Recommended Execution Order

1. QP stack overflow.
2. RoCE MKey layout and key format.
3. WQE completion flags.
4. RoCE publication gate.
5. Command outbox status.
6. Firmware startup/runtime pages.
7. Command sizes and capability parsing.
8. EQ mask, initial arm, and interrupt validation.
9. Event semantics, notifier locking, and health monitoring.
10. Ethernet IOKit lifecycle and quarantine ownership.
11. ConnectX-5 RX-only bring-up.
12. ConnectX-5 TX bring-up.
13. Ethernet error recovery, flow steering, and lifecycle soak.
14. Safe RoCE doorbell ABI and real PDs.
15. QP/MR/CQ/GID completion.
16. Correctness-first two-machine RoCE tests.
17. Performance features and additional hardware generations.

## 14. Completion Definition

The initial remediation project is complete when all of the following are true:

- The KEXT loads and unloads without panic on the selected ConnectX-5 test Mac.
- Firmware initialization includes page management and trustworthy command
  status handling.
- EQs are correctly masked, attached, and initially armed.
- One Ethernet SQ/RQ pair passes the full lifecycle and traffic matrix.
- Queue errors have a safe recovery or shutdown path.
- No hardware-referenced DMA memory is released before destroy success or
  bus-master disable.
- RoCE remains disabled unless its per-client isolation and doorbell ABI are
  complete.
- CI tests are reproducible from a fresh clone.
- Documentation reports verified status without overstating implementation
  maturity.
