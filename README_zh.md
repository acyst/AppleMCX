# AppleMCX.kext — 通用 Mellanox mlx5 家族适配器驱动

研究阶段的 macOS mlx5 驱动。首个受控硬件目标是 ConnectX-5 PF
以太网 bring-up；RoCE 代码存在，但默认禁止发布。

框架识别 ConnectX-4 到 ConnectX-8 及 BlueField 的若干 PCI ID，但目前没有
任何型号通过硬件认证。命令布局以 MLNX OFED 5.9 为准。

> 英文版见 `README.md`；实现状态见 `IMPLEMENTATION_STATUS.md`（英文）。

## 特性

- **Phase 1 核心实现**：固件页、outbox 状态、能力查询、EQ/MSI-X、UAR 页几何和健康定时器
- **默认 fail-closed**：以太网发布依赖能力验证；RoCE 默认关闭
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
- **用户态路径仍为原型**：缺少每客户端 UAR/DB 隔离，因此默认不发布直接映射路径
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
│   ├── libmlx/              用户态数据路径原型库
│   └── toolchain/           工具链 (libverbs/libmft + 工具)
├── tests/                   测试工具 (ibv_devinfo)
├── Tools/                   构建/签名/加载脚本
└── REMEDIATION_PLAN.md      安全 gate 与分阶段 bring-up 计划
```

## 构建

前置：macOS 13+，Xcode Command Line Tools。

```sh
make                        # 构建 KEXT (自动检测 arm64e/x86_64)
make tools                  # 构建用户态工具链
cd usermode/toolchain && make   # 或单独构建工具
make sign CODE_SIGN_ID="Apple Development: xxx@yyy.com"   # 签名
sudo make deploy            # 部署到 /Library/Extensions
sudo make load              # Phase 1 硬件 gate 打开前不要执行
make status                 # 验证
```

或一键脚本：

```sh
Tools/build_kext.sh
sudo Tools/load_kext.sh
```

### 无 macOS SDK 环境的 host 验证

```sh
make check-host
```

该命令验证纯 IFC 编码器和源码安全策略，不等同于 macOS SDK KEXT 构建或硬件验证。

## Apple Silicon 开发模式（加载 KEXT 必需）

KEXT 已被 Apple 弃用，在 Apple Silicon 上加载需要：

```sh
# 1. 开发者模式下关闭安全策略
sudo nvram boot-args="amfi_get_out_of_my_way=1"   # 仅开发环境!

# 2. 重启后验证
csrutil status

# 3. 签名使用开发证书 + entitlements (Tools/kext.entitlements)
```

> 本项目仅用于研究/实验室验证。`REMEDIATION_PLAN.md` 的 Phase 1 gate
> 打开前不得在真实硬件加载。

## PCI ID 匹配

plist 识别多个 mlx5 PCI ID；识别不代表已经通过固件、中断、DMA、生命周期或流量验证。

```
0x101315b3 0x101715b3 0x101b15b3 0x101d15b3 0x101f15b3
0x102115b3 0x102315b3 0xa2dc15b3 0xa2df15b3
```

启用其它型号：`Tools/gen_pci_match.sh` 生成全家族 IOPCIMatch 字符串后编辑 Info.plist。

### 型号支持矩阵

| 型号 | PCI ID | 支持状态 |
|------|--------|---------|
| ConnectX-4/4LX | 0x1013/0x1015 | PCI ID 已识别；未验证 |
| ConnectX-5/5Ex | 0x1017/0x1019 | 首个 PF 目标；尚未硬件验证 |
| ConnectX-6/6Dx/6LX | 0x101B/0x101D/0x101F | PCI ID 已识别；未验证 |
| ConnectX-7/8 | 0x1021/0x1023 | PCI ID 已识别；未验证 |
| BlueField-3/4 | 0xa2dc/0xa2df | PCI ID 已识别；未验证 |

> 型号通过完整验证矩阵前，不声明硬件支持。

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

工具包含 TCP 参数交换实现，但尚未使用本驱动完成任何 RDMA 流量验证。

## DMA 数据路径

```
发送: 映射后的用户数据 → WQE data_seg → 门铃 → 设备 DMA
接收: CQE 缓冲 (内核 DMA 一致) → CQC PAS → 硬件写
UAR:  默认 RoCE 发布 gate 禁止用户态直接映射路径
命令: 命令队列页 (withPhysicalMask)
EQ:   EQE 环形缓冲 (withPhysicalMask)
```

- 内核侧 `MlxDMA` 原型 pin 用户内存并返回设备可见 DMA 段
- QP 创建时 SQ/RQ 缓冲物理地址写入 QPC 的 wq_umem PAS
- CQ 创建时 CQE 缓冲用 DMA 一致内存

## 性能调优

DCQCN 拥塞控制参数通过 `IOConnectCallMethod(kMlxUCMethodCCModify)` 配置，
默认值: `rpg_min_dec_fac=256, rpg_ai_rate=5, rpg_time_reset=55, rpg_threshold=150`。
这些控制尚未通过硬件验证，用户态直接数据路径默认不启用。

## 状态

| 能力 | 状态 |
|------|------|
| Phase 0/1 代码实现 | 是 |
| Host tests | 通过 |
| 当前 P1 macOS SDK 构建 | 等待 CI |
| 硬件与流量 | 未验证 |
| RoCE 发布 | 默认关闭 |
| 型号认证 | 无 |

> 完整实现详情见 `IMPLEMENTATION_STATUS.md`（英文）。

## 参考资料

- 实现状态: `IMPLEMENTATION_STATUS.md`（英文）
- Linux 内核 `mlx5_core` 驱动源码（内核源码树）: https://github.com/torvalds/linux （drivers/net/ethernet/mellanox/mlx5_core、drivers/infiniband/hw/mlx5）
- NVIDIA MLNX_OFED（驱动发行包，`mlnx-ofed-kernel`）: https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/ — 文件 `MLNX_OFED_LINUX-5.9-0.5.6.0-ubuntu22.04-aarch64.tgz`

## 许可证

本项目基于 Linux 内核 `mlx5_core` 驱动移植，遵循 **GPL-2.0** 许可证，详见 `LICENSE`。
