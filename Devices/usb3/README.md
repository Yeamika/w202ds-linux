# w202ds-usb3-driver（USB / Type-C 接口）

```text
usb3/
├── build.mk
├── devicetree/
├── README.md
├── source/
│   ├── legacy-phy/          # 旧 companion PHY
│   ├── mainline-current/    # 当前 ssphy / IPA 电源域源码镜像
│   ├── vendor-reference/    # 原厂参考源码
│   └── w202-dp/             # USB-C DP Alt Mode 外接显示（w202_dp.ko）
└── userspace/               # 一线通 modprobe / udev 配置与 DR_Swap 脚本
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| USB 串口（ACM） | 有历史验证 | 当前完整内核下的完整 USB 回归待做。 |
| USB 网络（NCM） | 有历史验证 | 最新记录使用过 USB RE；长期传输与拔插稳定性待验。 |
| USB 3 SuperSpeed 主机 | 部分验证 | 经 USB-C 扩展坞接入的 SuperSpeed 设备在正反两个插入方向均枚举，并接入 USB 键鼠；吞吐量与长期稳定性未测。 |
| OTG 主机 / 设备模式切换 | 部分验证 | TCPM + dwc3 角色切换：作为受电方（sink）和供电方（source）时均有 USB host 记录；未接 PD 设备时为 USB device。旧的强制升压脚本不再使用。 |
| Type-C / PD 协商 | 部分验证 | 受电时 PD 输入限流 2000 mA 有记录；默认请求固定 9 V 2 A，但实际 9 V 合同未经测量确认。不提供 PPS、VCONN。与 `Devices/pmic` 的 Type-C 模块二选一，见下文。 |
| DP 外接显示 | 已验证（用户确认） | 单根 USB-C 线外接显示器输出画面由用户确认可用；USB3 + 2 lane DP（pin D），最高 HBR2，单个外接显示器。历史上出现过 `full link training failed: -7` 和 `flip_done timed out`，未逐一闭环。 |
| 一线通：边充电边用 USB3 + DP | 部分验证 | 同一连接中受电、DP HPD 与 SuperSpeed 枚举同时出现在日志中；长时间视频 + USB 负载 + 充电的持续验收未做。 |
| 4 lane DP（pin C / E） | 不支持 | 驱动只提供 pin D，pin C / E 请求被拒绝。 |
| HP Lapdock | 不支持 | 用户确认不可用；本仓不包含 Lapdock 专用脚本或规避措施。 |
| USB 网卡与 Wi-Fi 共存 | 待验证 | 历史问题尚未闭环。 |

更新日期：2026-11-08

## USB-C 一线通

一根 USB-C 线同时提供 USB3、DisplayPort 外接显示和给本机充电。组成：

| 部分 | 位置 | 说明 |
| --- | --- | --- |
| USB3 / DP combo PHY | 内核 `drivers/usb/phy/phy-sprd-ums9620-ssphy.c`，`include/linux/phy/phy-sprd-ums9620.h` | USB 与 DP 共享 PHY 引用，USB3 链路存活时不复位；DP 摆幅 / 预加重；TCA 方向与 mux 事务；有电池的板上 D+/D- 先交 PMIC BC1.2 检测。内建。 |
| DesignWare DP 库 | 内核 `drivers/gpu/drm/bridge/synopsys/dw-dp.c`，`include/drm/bridge/dw_dp.h` | 外部 PHY、控制器管理速率 / 均衡、HPD bit8、AUX 轮询、完全复位、掉电保护等可选平台钩子，`dw_dp_hw_reinit()`、`dw_dp_restore_stream()`。`dw-dp.ko`。 |
| UMP9620 Type-C / PD 端口控制器 | 内核 `drivers/usb/typec/tcpm/ump9620-tcpc.c` | 主线 TCPM 下的端口控制器，DP Alt Mode（pin D）、dwc3 角色、充电芯片 VBUS 与 PD 限流。默认关闭，需 `enable=1`。`ump9620-tcpc.ko`。 |
| 充电芯片电源路径 | 内核 `drivers/power/supply/bq256xx_charger.c` | `otg-vbus` 稳压器、PD 输入电流 / 电压 / OVP、双芯片并联充电。`bq256xx_charger.ko`。 |
| DP 外接显示驱动 | `source/w202-dp/` | DPU1 + DPTX 胶水驱动，独立 DRM 设备 `w202-dp`。目标 `w202ds-usb3-dp`，见 [说明](source/w202-dp/README.md)。 |
| 用户态 | `userspace/` | 启用端口控制器、禁止旧 `sc27xx_typec`、PD 显示器 / 扩展坞的 DR_Swap 脚本与 udev 规则、可选 GNOME Mutter 规则，见 [说明](userspace/README.md)。 |

内核配置见 `config/fragments/usb-single-cable.config`。设备树沿用本目录 DTSI 中已有的 DPU1、DPTX（GPIO120）、USB3 / ssphy 节点，以及 `pmic`、`charging` DTSI 中的 Type-C / PD 节点和 `otg-vbus` 稳压器，无需新增节点。DP 无需固件。

### 与 PMIC Type-C 和充电模块的关系

本方案与现有两个外部模块绑定相同的设备树节点，**同一系统只能启用一套**：

| 节点 | 本方案 | 现有模块 |
| --- | --- | --- |
| `pd@e00`（`sprd,ump9620-pd`） | `ump9620-tcpc.ko` | `Devices/pmic` 的 `w202_ump9620_typec.ko`（仅 sink，PPS） |
| `charger@6b`（`ti,bq2560x_chg`）及 `charger2@6b` | `bq256xx_charger.ko`（同时直接控制从芯片） | `Devices/charging` 的 `w202_bq_control.ko` |

`ump9620-tcpc` 需要名为 `bq256xx-charger` 的电源和其 `otg-vbus` 稳压器，否则 probe 一直延迟；`w202_bq_control` 不提供二者。现有模块提供 PPS 快充（最高 9.8 V 实测），本方案只请求固定 5 V / 9 V，但提供双角色、DR_Swap 后的 USB host 和 DP Alt Mode。哪一套作为默认、是否合并，由维护者决定。

### 已知限制

- 仅 pin D：USB3 + 2 lane DP，最高 HBR2；无 MST、DP 音频、HDCP。
- 两颗 redriver（PI3DPX1217、PTN38003A）ID 读取均无应答，方向依靠 GPIO120；redriver 是否上电未确认。
- 外接显示的帧缓冲来自 4 GiB 以下 CMA，与内屏共享。我们的部署使用 `cma=256M`，默认 64 MiB 下高分辨率是否够用未验证。
- DR_Swap 脚本只尝试一次；TCPM 不会自动发起。
- KDE 下的外接显示未验证；Mutter 规则仅适用于 GNOME。
