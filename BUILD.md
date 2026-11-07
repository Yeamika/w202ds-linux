# 构建

完整内核和模块在本地构建。需要 GNU make、Python 3.11+、AArch64 GCC/binutils、flex、bison、bc、OpenSSL/ELF 开发包、dtc、kmod。以下从仓库根运行，输出全部放 `output/`。

```sh
export LOCALVERSION=
mkdir -p output/kernel
cp config/server.config output/kernel/.config
make -C kernel O="$PWD/output/kernel" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C kernel O="$PWD/output/kernel" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j4 Image modules
make -C Devices KDIR="$PWD/output/kernel" OUT="$PWD/output/modules" all
mkdir -p output/devicetree
dtc -I dts -O dtb -o output/devicetree/w202ds-server.dtb devicetree/w202ds-server.dts
```

输入为 `kernel/` 原生完整源码、`config/server.config`、`Devices/*/source/` 与板级 DTS。
输出为 `output/kernel/arch/arm64/boot/Image`、树内模块、`output/modules/<目标>/*.ko` 及逐目标 `build.json`、DTB。
构建不安装、不加载模块、不刷写设备。完整 `modules` 步骤生成树内导出符号，不能以 `modules_prepare` 替代。

`LOCALVERSION` 显式置空用于禁止 Git 提交自动附加 `+`；版本后缀仍取 `config/server.config`
中的 `CONFIG_LOCALVERSION="-w202ds"`。对子模块源码构建时也保持此设置。

现有 Server 配置强制使用自身命令行，入口为 `rdinit=/w202diag`，需要另行提供匹配 initramfs/rootfs。
这份配置和源码不是通用发行版安装器，也不包含任何引导固件。板级 DT 中继承的硬件/保留内存必须与启动环境一致。

使用现有 CM2 冻结构建目录进行单模块开发时，见 [独立模块构建说明](Devices/BUILD.md) 的 cfg80211 导出选项。
构建检查与真机功能验收分别记录，不能由编译成功推断硬件完整可用。

2026-11-05 的完整构建已在 Server 上机。随后按用户要求只保留下一次构建的 `CONFIG_LOCALVERSION="-w202ds"`；当前运行版本仍为 `6.18.35-w202ds-m23-wifi`。更换 release 时还须同步 initramfs、模块目录和用户态版本约定，不能只改 Image 文件名。

基带用户态独立入口为 `make -C Devices/modem/userspace OUT=绝对输出目录 all`，不触发内核构建。目录名称已简化，内核模块目标名称不变。

## 分离后的内建与模块

`kernel/` 内 5 个功能位置的相对链接指向 `Devices/`；复制源码时保留整个 `6.18.35` 布局和链接。
早期黑框日志、背光、USB PHY、旧诊断 fbcon 和恢复接口仍按 Server 配置内建，模块归属不等于晚加载。
背光、USB PHY 另有显式 `.ko` 编译目标，见 `Devices/BUILD.md`。

内核 Panfrost 是原版，板级版本仅在 `Devices/gpu/source/panfrost/` 维护。
安装时外部板级模块放到 `updates/w202ds/`，重建 depmod 和 initramfs 并核实解析路径；
不要只安装树内原生 `panfrost.ko`。本仓构建命令不执行安装。

默认诊断配置保留 120 秒恢复计时；普通系统可设 `CONFIG_W202DS_DIAG_BOOT_TIMEOUT=0`，
保留 `/dev/w202diagctl` 用户态兼容。新的标准 PMIC 重启路径、显示顺序和 USB 需要真机回归。
配套使用本仓 DTSI 编译的 DTB，不能依赖已删除的启动时 GPU FDT 修补。

USB-C 一线通需要 `config/fragments/usb-single-cable.config`（dw-dp、UMP9620 Type-C/PD、DP Alt Mode、bq256xx），在 `olddefconfig` 前以 `kernel/scripts/kconfig/merge_config.sh -m -O output/kernel output/kernel/.config config/fragments/usb-single-cable.config` 合入。
其中 bq256xx 与 `Devices/charging` 模块绑定同一充电芯片节点，两套充电驱动择一，见 [USB / Type-C 接口](Devices/usb3/README.md)。
