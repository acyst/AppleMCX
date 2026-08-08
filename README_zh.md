# AppleMCX.kext — 通用 Mellanox mlx5 家族适配器驱动

macOS (Apple Silicon) 上的 Mellanox ConnectX 系列网卡驱动，提供 **RoCEv2**（RDMA over Converged Ethernet）协议服务。

**通用框架**：覆盖整个 ConnectX 家族（ConnectX-4 ~ ConnectX-8 + BlueField-3/4），首个实现目标为 **ConnectX-5 (mcx5)**。所有硬件结构体精确复刻 Linux `mlx5_core`，类名统一为 `Mlx` 前缀。

> 英文版见 `README.md`；实现状态见 `IMPLEMENTATION_STATUS.md`（英文）。

## 特性

- **RoCEv2 完整**：RC QP 状态机、CQ 完成事件、MR/DMA 零拷贝、AH/GID 编址、DCQCN 拥塞控制
- **全家族适配**：CX4-8 + BlueField，型号工厂分派链，新增型号只需补 PCI ID
- **多设备支持**：`deviceName` 属性 + `mlx_list_devices` + verbs 多设备枚举
- **IB 链路层预留**：MlxPortType/IB 编址/AH IB 分支
- **异步事件闭环**：EQ 事件 → MlxUserClient → libmlx → `ibv_get_async_event`
- **perftest 真实握手**：TCP 交换 QPN/RKEY/PSN/GID 后进入数据循环

## 架构

```
IOPCIDevice (ConnectX-5)
  → MlxPCIDriver (核心层: 命令接口/事件队列/UAR/固件初始化)
      → 发布 nub (mlx_rdma, mlx_eth)
          → MlxRoCE (verbs 协议层: QP/CQ/MR/AH/GID/DCQCN)
          → MlxEthernetDriver (以太网接口, IB 端口自动跳过)
  → IOServiceOpen → MlxUserClient → 用户态 libmlx
```

## 关键设计

- **单 KEXT 双/三 IOService**：模拟 Linux auxiliary bus 分层，避免跨 KEXT 依赖
- **硬件解耦**：所有子模块经 `MlxHCA` 抽象接口访问硬件，新增型号只需补 PCI ID + 能力差异
- **零拷贝数据路径**：用户态直接 mmap UAR 写门铃 + 直读 CQE，RDMA 收发无系统调用
- **DCQCN 纯固件闭环**：驱动只封装 `MODIFY_CONG_PARAMS` 命令

## 目录结构

```
mlx-kext/
├── LICENSE                  GPL-2.0
├── README.md                英文 README
├── README_zh.md             本文件（中文 README）
├── IMPLEMENTATION_STATUS.md 实现状态总览（英文）
├── Makefile                 构建入口 (KEXT + 工具链)
├── AppleMCX.kext/            KEXT 包 (Info.plist + MacOS/AppleMCX)
├── Sources/
│   ├── hw/                  硬件结构 (MlxRegs/WQE/门铃/HCA 抽象 + CX4-7)
│   ├── core/                核心层 (MlxPCIDriver/Cmd/EQ/UAR/Health/DMA)
│   ├── ib/                  verbs 层 (RoCE/QP/CQ/MR/AH/GID/CC)
│   ├── netif/               以太网接口 (MlxEthernet)
│   └── userclient/          IOUserClient + MlxUCIO.h (用户态共享头)
├── usermode/
│   ├── libmlx/              零拷贝用户态库
│   └── toolchain/           工具链 (libverbs/libmft + 工具)
├── tests/                   测试工具 (ibv_devinfo)
├── Tools/                   构建/签名/加载脚本
└── build/stub/              内核 API 模拟 (无 SDK 环境语法检查)
```

## 构建

前置：macOS 13+，Xcode Command Line Tools。

```sh
make                        # 构建 KEXT (自动检测 arm64e/x86_64)
make tools                  # 构建用户态工具链
cd usermode/toolchain && make   # 或单独构建工具
make sign CODE_SIGN_ID="Apple Development: xxx@yyy.com"   # 签名
sudo make deploy            # 部署到 /Library/Extensions
sudo make load              # 加载
make status                 # 验证
```

或一键脚本：

```sh
Tools/build_kext.sh
sudo Tools/load_kext.sh
```

### 无 macOS SDK 环境的编译期验证

`build/stub/` 模拟内核 API（IOKit/libkern），在无 SDK 环境下做语法/类型检查：

```sh
clang++ -std=c++17 -fno-exceptions -Ibuild/stub -ISources -ISources/hw \
  -ISources/core -ISources/ib -ISources/netif -ISources/userclient \
  -fsyntax-only <file>.cpp
```

## Apple Silicon 开发模式（加载 KEXT 必需）

KEXT 已被 Apple 弃用，在 Apple Silicon 上加载需要：

```sh
# 1. 开发者模式下关闭安全策略
sudo nvram boot-args="amfi_get_out_of_my_way=1"   # 仅开发环境!

# 2. 重启后验证
csrutil status

# 3. 签名使用开发证书 + entitlements (Tools/kext.entitlements)
```

> ⚠️ 本项目定位为研究/实验室验证。生产分发需评估 Apple 对 KEXT 的限制。

## PCI ID 匹配

默认启用全家族 mlx5 适配器：**ConnectX-4/5/6/7/8** 及 **BlueField-3/4**。

```
0x101315b3 0x101715b3 0x101b15b3 0x101d15b3 0x101f15b3
0x102115b3 0x102315b3 0xa2dc15b3 0xa2df15b3
```

启用其它型号：`Tools/gen_pci_match.sh` 生成全家族 IOPCIMatch 字符串后编辑 Info.plist。

### 型号支持矩阵

| 型号 | PCI ID | 支持状态 |
|------|--------|---------|
| ConnectX-4 | 0x1013 (PF), 0x1014 (VF) | ✅ 已适配 |
| ConnectX-4LX | 0x1015 (PF), 0x1016 (VF) | ✅ 已适配 |
| ConnectX-5 | 0x1017 (PF), 0x1018 (VF) | ✅ 已适配 |
| ConnectX-5Ex | 0x1019 (PF), 0x101A (VF) | ✅ 已适配 |
| ConnectX-6 | 0x101B (PF), 0x101C (VF) | ✅ 已适配 |
| ConnectX-6 Dx | 0x101D (PF), 0x101E (VF) | ✅ 已适配 |
| ConnectX-6 LX | 0x101F | ✅ 已适配 |
| ConnectX-7 | 0x1021 (PF), 0x1022 (VF) | ✅ 已适配 |
| ConnectX-8 | 0x1023 | ✅ 已适配 |
| BlueField-3 (CX7) | 0xa2dc | ✅ 已适配 |
| BlueField-4 (CX8) | 0xa2df | ✅ 已适配 |

> 驱动代码对所有 ConnectX 型号通用（与 Linux mlx5_core 一致），差异仅在
> PCI ID、固件能力寄存器、ISSI 协商（ConnectX-4 老固件可能仅支持 ISSI=0，
> 驱动已自动回退）。

## 测试工具

```sh
cd usermode/toolchain && make

./ibv_devinfo    # 查看 RDMA 设备能力
./ib_write_bw    # RDMA Write 带宽 (服务端/客户端, TCP :18515 握手)
./ib_send_bw     # SEND 带宽
./ib_read_bw     # RDMA Read 带宽
./ib_write_lat   # RDMA Write 延迟
./mlxconfig      # 固件配置查询/设置
./mlxstatus      # 固件状态
./mlxlink        # 链路状态
./mlxreg         # 寄存器读写
./mlnx_qos       # RoCE QoS (PFC/优先级映射)
```

perftest 系工具通过 TCP `:18515` 双端交换 QPN/RKEY/PSN/GID 后完成 QP 状态机
（RST→INIT→RTR→RTS），RDMA Write/Read 使用对端 RKEY。

## DMA 数据路径

```
发送: 用户数据 → VirtToPhys → WQE data_seg.addr(物理) → 门铃 → 硬件 DMA
接收: CQE 缓冲 (内核 DMA 一致) → CQC PAS → 硬件写
UAR:  用户态 mmap (clientMemoryForType) → 直写门铃
命令: 命令队列页 (withPhysicalMask)
EQ:   EQE 环形缓冲 (withPhysicalMask)
```

- 内核侧 `MlxDMA` pin 用户内存获取物理地址（`MlxDMAReq` 含每段长度，精确转换）
- QP 创建时 SQ/RQ 缓冲物理地址写入 QPC 的 wq_umem PAS
- CQ 创建时 CQE 缓冲用 DMA 一致内存

## 性能调优

DCQCN 拥塞控制参数通过 `IOConnectCallMethod(kMlxUCMethodCCModify)` 配置，
默认值: `rpg_min_dec_fac=256, rpg_ai_rate=5, rpg_time_reset=55, rpg_threshold=150`。
数据路径零拷贝: 用户态直写 UAR 门铃 + 直读 CQE，无系统调用。

## 状态

| 能力 | 状态 |
|------|------|
| 命令接口/设备初始化/固件握手/EQ/UAR/健康轮询 | ✅ |
| 以太网接口 (TX/RX) | ✅ |
| RoCEv2 + RC 数据路径 (post_send/poll_cq) | ✅ |
| 异步事件: EQ→用户态 `ibv_get_async_event` 全链路闭环 | ✅ |
| perftest 真实 TCP 握手 (QPN/RKEY/PSN/GID) | ✅ |
| 多设备 (deviceName / mlx_list_devices) | ✅ |
| IB 链路层预留 (IB 编址 / AH IB 分支) | ✅ |

> 完整实现详情见 `IMPLEMENTATION_STATUS.md`（英文）。

## 参考资料

- 实现状态: `IMPLEMENTATION_STATUS.md`（英文）
- Linux 内核 `mlx5_core` 驱动源码（内核源码树）: https://github.com/torvalds/linux （drivers/net/ethernet/mellanox/mlx5_core、drivers/infiniband/hw/mlx5）
- NVIDIA MLNX_OFED（驱动发行包，`mlnx-ofed-kernel`）: https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/ — 文件 `MLNX_OFED_LINUX-5.9-0.5.6.0-ubuntu22.04-aarch64.tgz`

## 许可证

本项目基于 Linux 内核 `mlx5_core` 驱动移植，遵循 **GPL-2.0** 许可证，详见 `LICENSE`。
