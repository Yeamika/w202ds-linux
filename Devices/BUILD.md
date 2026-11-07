# 独立模块构建

`Makefile` 汇总各硬件 `build.mk`；`build-module.py` 每次构建一件 `.ko`，并核验内核配置、release、符号提供者、源文件和模块格式。
工程没有对 LK、EDK2 或其他工作区目录的构建依赖。
硬件目录使用简短名称，例如 `hall/`；分组目标仍为 `w202ds-hall-driver`，独立模块目标仍使用 `w202ds-<硬件>-<子驱动>`，可通过 `list` 查看。目录改名不改变构建目标或 `.ko` 名称。

```sh
export LOCALVERSION=
make -C Devices list
make -C Devices KDIR="$PWD/output/kernel" OUT="$PWD/output/modules" w202ds-hall-driver
make -C Devices KDIR="$PWD/output/kernel" OUT="$PWD/output/modules" w202ds-drm-driver
make -C Devices KDIR="$PWD/output/kernel" OUT="$PWD/output/modules" all
```

每个目标输出自己的模块、`Module.symvers`、`build.log` 和 `build.json`；提供者按实际符号依赖先构建。
`all` 包含当前 Server 模块及已验证的 Linux 基带启动准备。诊断、GPU frequency、历史触控为显式目标。
目标能独立编译不代表它能独立运行；例如 SIPC、Trusty、WCN 共享硬件状态，不能据此随意热卸载。

完整新内核先构建 `Image modules`，使用全局 `Module.symvers`，无需额外 provider。
当前 CM2 冻结内核输出没有收集 cfg80211 导出，可显式使用已核对的收据：

```sh
make -C Devices \
  KDIR=/root/bsp/w202ds/output/handoff-20261105/linux-build \
  OUT="$PWD/output/frozen-modules" \
  WCN_KERNEL_PROVIDER=wcn/kernel-providers/server-20261105/cfg80211.json w202ds-wcn-wifi
```

该 provider 限定配置/全局符号表/release/实际 cfg80211.ko 的哈希；不能用于别的内核配置。
它的符号来自原 M23 私有构建，最新输出里的 cfg80211.ko 与原模块逐字节相同。
历史构建记录仅在本地保留；新构建使用实际源码、内核输出及显式指定的 provider。


额外的独立测试目标：`w202ds-drm-backlight`、`w202ds-usb3-legacy-phy`。
二者在当前 Server 内核内建；独立 `.ko` 用于验证模块模式，不能在同一内建实现上重复加载。
只有 DTSI 的硬件目录暂不注册空模块目标，构建进度以各目录 README 为准。

`w202ds-drm-handoff` 独立构建可选 `pj4edk2-handoff.ko`，不依赖 DRM 构建目标。

`w202ds-usb3-dp` 构建 USB-C 外接显示 `w202_dp.ko`，需要内核按 `config/fragments/usb-single-cable.config` 构建 `Image modules`（dw-dp、ssphy DP 导出由全局 `Module.symvers` 提供）；不在 `all` 中，需显式指定。
