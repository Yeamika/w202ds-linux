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
  --lk-baseline /path/to/inputs/uboot-b-pjs-ram-v2.img \
  --settings-image /path/to/inputs/settings-source.img \
  --touch-firmware /path/to/Himax_firmware_guoxian.bin
```

## 目录职责

- `pocketjs4edk2_uefi/`：组合层源码、EFI 启动器、测试和构建工具；内部
  `components/` 以子模块固定 `edk2-ums9620` 与 `pj4edk2`。
- `lk/`：当前使用的 LK 桥接和 RAMBoot 源码，也是后续原厂 LK 及板级 LK
  参考代码的存放位置。本目录不参与 ESP 独立编译，由组合构建通过链接使用。

历史验证记录在 [`pocketjs4edk2_uefi/VALIDATION.md`](pocketjs4edk2_uefi/VALIDATION.md)。
