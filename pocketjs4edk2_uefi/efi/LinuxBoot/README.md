# LinuxBoot

W202DS 独立 Linux EFI 加载器。同一份源码构建 `DebianServer.efi` 与 `DebianKde.efi`，从各自 ESP 目录读取 Linux 启动包，退出 UEFI 后进入原始 ARM64 内核，不恢复或调用 LK。

## 输入与构建

由 `efi/PocketJsUefiPkg.dsc` 独立构建，两个 EFI 应用不嵌入平台固件卷。组合构建将应用安装为 `EFI/DebianServer/bootaa64.efi` 和 `EFI/DebianKDE/bootaa64.efi`，对应目录另含 `boot.img`、32字节 `payload.bin`；共用 `EFI/W202DS/LINUX.DTB`。

启动包由外层从匹配的 Android v4 Linux boot 镜像提取，内核保持原字节，initramfs 分别固定选择 Server 或 KDE。当前内核 `6.18.35-w202ds-m23-wifi` 无 EFI stub 且强制使用内置命令行，因此根系统选择写在 initramfs，不能只靠 EFI 命令行参数。

## Features

- 校验 ESP GUID、文件长度、ARM64 内核重定位条件、DT 内存/保留区及各载荷 CRC；CRC 只检测传输损坏，不是安全启动签名。
- 内核放到 `0xd4000000 + text_offset`，initrd 放 `0xd0000000`，DT 放 `0xd8000000`；要求平台映射完整 `0xd0000000..0xd81fffff` 暂存区，该区不加入 UEFI 分配器。
- 交接前关闭文件，成功 ExitBootServices 后停止原厂派生入口遗留的 PMIC 看门狗计数，再完成缓存/MMU交接。此内核带自定义 `ump9620-wdt` 驱动，不能依赖其稍后的 probe 处理交接前已运行的计数器。
- `/chosen/w202,uefi-profile` 标识本次 EFI 选择；只读 ESP，不更改 misc 或 A/B 分区后缀。
- `/chosen/w202,uefi-load-us` 保存六个大端 u64 阶段耗时（微秒）：元数据/DTB/头部、内核读取、内核校验及清零、initrd 读取、initrd 校验、DT 准备。供启动后回收分析，不在每次读取时同步输出进度；不包含应用启动前的页面动画和 ExitBootServices 后的内核启动。

## Experimental / TODO

2026-11-05 两个独立应用、配套平台和5 MiB容器已构建，四项载荷解析测试和PMIC看门狗事务测试通过。新平台目前仅RAM启动，尚未刷入B；KDE尚未测试。

Server分阶段启动已进入真实 `/dev/mmcblk0p78` ext4根（`server-20261027-v1 / server/normal`），`w202,uefi-profile=server` 已确认。USB SSH与5 GHz STA Wi-Fi SSH同时连通，持续超过5分钟；显示/GPU、键盘/霍尔/息屏、电量与双路停充、VPU服务均启动正常。这里只验证服务启动与电源读回，不替代各硬件功能的完整验收。基带foundation单独加载也成功。

完整 Server 的基带已由 Linux 模块恢复：AVB 缓存准备、CP RAM/PHY准备、SIPC设备内存映射与原厂签名校验通过；新启动自动附着、蜂窝HTTPS成功。初始化归属基带项目，本EFI没有加入基带寄存器操作。测速与具体证据见基带README。

页面通过公开 `BootDisplay` 配置表传入黑框区域；本加载器核对1920×1200逻辑坐标、90度旋转、GOP RGB565模式、实际DPU扫描地址与保留内存后，转换为 `/chosen/pocketjs,boot-display`。没有本次区域时删除旧记录；不猜测硬编码页面布局。内核支持旧LK的1280×800及当前UEFI的1920×1200坐标。当前源码、构建、边界和栅格测试通过，完整真机交接正在验证。

继续验收显示交接、KDE、关机/重启及B常驻安装。当前Server仍有诊断性的默认Android设置恢复；最终需还原正式选择策略与原始设置。当前DT来自本机已验证原生Linux的完整DT快照；动态发现完整内存、其他板型与其他内核格式尚未支持。

## 来源

EFI 文件访问与生命周期采用项目内 EDK II 协议。原始 ARM64 交接基于本项目 AndroidBoot 的已测试交接例程；PMIC ADI 事务参照本项目 BoardServicesDxe 和原厂 LK 的 UMP9620 看门狗定义。

## 公共层归属

本目录继续作为唯一 W202DS Linux EFI 加载器。内部的 [common](common/README.md)
固定 pj4edk2 `linux-efi` 分支；远程通过 gitlink 固定提交，本地为相对符号链接。
不会在 components 下再建立另一套 Linux EFI 实现。

拆分方案见 [common/ABSTRACTION.md](common/ABSTRACTION.md)：公共镜像/DT/显示协议
处理逐步抽出；ESP 与启动策略、加载布局、GOP/DPU、PMIC 看门狗及硬件交接留在此项目。
当前 common 仅有初始化 MD，INF 与 C 源码尚未改为调用公共实现，现有加载器行为不变。
