# W202DS LK

## 目录

- `firmware/uboot-mod.bin`：v6 解锁工具包自带的解锁镜像，当前遗留构建链的默认起点；来源见 `legacy-buildchain/ramlk/README.md`。
- `vendor_source/`：`sprd-linux/lk` 上游参考源码子模块，固定到 `47f4d04465edac8978a819f969b423e741a5a590`；不参与当前 IMG 遗留链构建。
- `source/`：目标 LK 完整源码的位置。目前为空；当前基于 IMG 补入加载器，没有可完整重建该底包的目标 LK 源码。
- `legacy-buildchain/ramboot/bridge/uefi_ramboot.c/.h`、`legacy-buildchain/ramboot/`：我们可重编译的桥接、加载器、返回逻辑、探针、封装脚本和测试。这些不是完整目标 LK 源码。

## 上游参考源码

参考仓库：https://github.com/sprd-linux/lk.git

历史桥接来源记录的基准提交（当前参考上游未提供此提交，不作为子模块版本）：`cacbc68e183a69878aac90df2fb08ac1596a0645`。
本仓库保留 W202DS 的本地修改。参考源码不能据此认定能重建当前设备的原厂 LK 或 RAMLK 底包。

最小 LK 构建入口为 `legacy-buildchain/Makefile` / `build.py`，输出 `lk-debuger.img`；支持可关闭的看门狗和 Fastboot 自动恢复。
完整 UEFI 组合入口为 `pocketjs4edk2_uefi/build.py` 和 `build-firmware.py`，复用同一最小 LK 构建器。
输入校验记录见 `pocketjs4edk2_uefi/inputs.lock.json`。
固件二进制被 Git 忽略，迁移工程时须携带 `lk/firmware/uboot-mod.bin`。

`legacy-buildchain/ramlk/` 从工具包的 `firmware/uboot-mod.bin` 开始，先补零至 5 MiB，输出相同的 `uboot_a.img` 和 `uboot_b.img`；A 用于 Android，B 再重建历史 PJB RAMBoot v2；不再读取设备 `uboot_b.img` 备份。重新编译后最终镜像与现用底包哈希及字节一致，详见该目录 README。

启动设置由 pj4edk2 保存到 ESP 的 `EFI/BOOT/pj4edk/settings0.bin`、`settings1.bin`。构建不再读取或植入 PJBINFO1 尾部 bank。
