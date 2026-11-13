# w202ds-boot

W202DS 引导固件仓库。当前布局把可构建的组合工程放在
[`pocketjs4edk2_uefi/`](pocketjs4edk2_uefi/README.md)，仓库根只保留板级 LK 材料。

```text
w202ds-boot/
├── pocketjs4edk2_uefi/   # 组合构建：EDK II + PocketJS + ESP + LK 容器
└── lk/                   # LK 桥接、原厂 LK 与 LK 参考代码
```

## 快速开始

远端克隆并初始化两个组件子模块：

```sh
git clone --recursive https://github.com/Yeamika/w202ds-boot.git
```

完整构建说明、组件查找顺序、ESP 源码布局和产物列表见
[`pocketjs4edk2_uefi/README.md`](pocketjs4edk2_uefi/README.md)。

```sh
python3 pocketjs4edk2_uefi/build.py --output /path/to/build/w202ds \
  --lk-baseline /path/to/inputs/lk-jump-rmk.img
```

## 目录职责

- `components/`：以子模块固定 `edk2-ums9620` 与 `pj4edk2`，本地开发可在同一路径使用链接。
- `pocketjs4edk2_uefi/`：组合层源码、EFI 启动器、测试和构建工具。
- `pocketjs4edk2_uefi/tools/verify_firmware.py`：W202DS 固件格式、LK 分区布局、嵌入数据和尾部校验；两个构建入口完成构建后自动运行。REIMUKS 只调度构建、收集并核对产物身份。
- `lk/`：当前使用的 LK 桥接和 RAMBoot 源码，也是后续原厂 LK 及板级 LK
  参考代码的存放位置。本目录不参与 ESP 独立编译，由组合构建通过链接使用。

历史验证记录在 [`pocketjs4edk2_uefi/VALIDATION.md`](pocketjs4edk2_uefi/VALIDATION.md)。

## 本地二进制输入

RAMLK 从 `lk/firmware/uboot-mod.bin` 自动生成；设置由运行时读写 ESP 上的 `EFI/BOOT/pj4edk/settings0.bin` 和 `settings1.bin`。两个构建入口均使用
本仓库内的 LK 底包，无需 REIMUKS 或 Linux 工作区提供二进制。
例如只需指定新输出目录：

```sh
python3 pocketjs4edk2_uefi/build.py --output /path/to/build/w202ds
```

二进制当前由 Git 忽略；复制工程时需携带 `lk/firmware/uboot-mod.bin`。
仅克隆 Git 仓库不会自动得到它们。Git 源码依赖及编译工具仍需正常准备。

## 离线校验

```sh
python3 pocketjs4edk2_uefi/tools/verify_firmware.py /path/to/build/w202ds \
  --manifest firmware-manifest.json
python3 -m unittest discover -s pocketjs4edk2_uefi/tests -p test_boot_partition.py -v
```

`build-firmware.py` 生成 `firmware-manifest.json`；独立 `build.py` 的清单为 `manifest.json`，校验时按对应入口选择。

分区校验测试默认使用确定的文件夹具；设置 `W202_FIRMWARE_BUILD=/path/to/build/w202ds` 可改用完整构建产物。测试只读输入，在临时副本上做损坏检查，不刷写设备。
