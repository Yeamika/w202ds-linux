# w202ds-suspend（系统挂起唤醒策略）

```text
suspend/
├── build.mk
├── source/ums9620-lpintc/      LP_AP_INTC 唤醒表模块（来自 mu300-linux，6.18 适配 + 严格挂起唤醒）
├── userspace/80-w202-strict-wakeup.rules
└── README.md
```

目标：`mem`（deep）挂起后只由**电源键**和 **RTC 闹钟**唤醒；触摸、霍尔、键盘拆装、
充电器 / 电量计 / Type-C PD、Wi-Fi PCIe、IPA、基带邮箱都不再把平板叫醒。三处配合：

| 位置 | 内容 |
| --- | --- |
| 内核 `drivers/gpio/gpio-sprd.c`、`gpio-eic-sprd.c`（kernel 子模块） | 两个 irq_chip 加 `IRQCHIP_MASK_ON_SUSPEND`。AP EIC / GPIO 汇总中断在 TF-A 的低功耗中断表里是系统唤醒线，原来只有 `IRQCHIP_SKIP_SET_WAKE`，`suspend_device_irqs()` 只做惰性禁用、硬件仍未屏蔽，任何边沿（触摸、霍尔、键盘检测、Wi-Fi 唤醒 GPIO）都会从 deep 挂起唤醒。现在没有被 `enable_irq_wake()` 武装的线在挂起时由核心屏蔽，已武装的照常工作。 |
| `ums9620_lpintc.ko` | 运行时：把 Linux 已在 GIC 打开的 SPI 补进 LP_AP_INTC `IRQ_EN`（簇掉电时可被这些中断唤醒，原 mu300 功能）。系统挂起（syscore）时：整张表收窄为参数 `suspend_wake_spi`（默认 230，即 INTC7 bit 6 的 PMIC sc27xx，承载电源键与 RTC 闹钟），同时在 GIC 分发器里关闭其余 SPI——`IRQF_NO_SUSPEND` 的 CP 邮箱会回应 AP 的睡眠通知，若保持使能会在 PSCI CPU_SUSPEND 时挂起并立即返回；恢复时整表与 GICD 使能位原样写回，期间锁存的中断在恢复后送达。 |
| `80-w202-strict-wakeup.rules` | 关闭 `bq256xx-charger`、`sc27xx-fgu`、`tcpm-source-psy-*`、PCIe `0000:11:00.0`（Wi-Fi）和 `25220000.sipa` 的 `power/wakeup`：运行时仍是唤醒源（wakeup source），但不再作为系统唤醒设备。 |

模块来源：`dikeckaan/mu300-linux` `upstream/modules/ums9620_lpintc`（833616a6c03a），按 6.18 的
`struct syscore_ops` 接口适配（`register_syscore_ops`），再加入严格挂起唤醒部分；源文件
GPL-2.0-only 声明保留。只接受 UMS9620。

## 构建与安装

```sh
make -C Devices KDIR=/path/to/prepared/kernel OUT=/path/to/modules w202ds-suspend-driver
```

模块装到 `/usr/lib/modules/$(uname -r)/updates/w202ds/` 并 `depmod`；开机加载用
`/etc/modules-load.d/w202-lpintc.conf`（一行 `ums9620_lpintc`）。需要放行别的唤醒源时
设模块参数，例如 `options ums9620_lpintc suspend_wake_spi=230,<SPI>`。udev 规则装到
`/etc/udev/rules.d/`。内核侧补丁见 kernel 子模块分支 `suspend-wake`。

## 实机验证（2026-11-09，内核 `6.18.35-w202ds-m23-audio-20261109`，GNOME 48）

- `/sys/power/mem_sleep` 为 `[deep]`，`suspend_wake_spi` = 230。
- RTC 闹钟 30 秒、60 秒、90 秒三次挂起：均睡满设定时间后由 RTC 唤醒，恢复正常；
  挂起期间触摸屏幕不会唤醒（用户实机确认）。
- 上述电源、PD、电量计设备的 `power/wakeup` 读回 `disabled`。PCIe `0000:11:00.0` 当前读回
  `enabled`（驱动探测后可能重新打开），其中断在挂起时按设计已由 `ums9620_lpintc` 从唤醒表和
  GICD 中移除；Wi-Fi 唤醒未单独测试。

未验证：长时间（数小时）挂起耗电、充电器插拔时挂起、Wi-Fi / 蜂窝在线时的挂起恢复、
来电或短信唤醒（当前设计下不会唤醒）。

更新日期：2026-11-09
