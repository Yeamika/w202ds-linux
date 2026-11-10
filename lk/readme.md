# LK 材料

本目录保存 W202DS 板级 LK 相关材料：

- `uefi_ramboot.c/.h` 与 `ramboot/`：当前使用的 LK 驻留/返回桥接、加载器、
  探针和打包脚本，由 `pocketjs4edk2_uefi/build.py` 调用。
- 原厂 LK 代码、LK 参考实现和后续板级 LK 源码也放在本目录；整理前不要与
  `pocketjs4edk2_uefi/` 的源码混合。

当前组合构建只要求可重建的桥接部分和外部固定 LK 底包
`uboot-b-pjs-ram-v2.img`；完整原厂 LK 源码重建流程尚未打通。底包哈希和用途见
`pocketjs4edk2_uefi/inputs.lock.json`。
