# AndroidLk

实验性 Android EFI 入口。消费 LK 在进入 EDK2 前保存的返回描述符，退出 UEFI 服务后恢复同一次启动中的原厂 LK，再由 LK 加载 Android A。它不是把 LK 二进制当作 PE/EFI 文件，也不从 LK 冷入口重启初始化。

## Features（已验证）

- 描述符检查、块设备 Flush、ExitBootServices 与无栈缓存/MMU交接。
- 2026-11-05 RAM 往返测试：LK → EDK2 → 本应用 probe → 原 LK Fastboot，约315 ms，无重启；原生 USB 状态与512 KiB日志读取正常。
- 保留日志包含 `ANDROID_LK ... action=0`、返回阶段 `0x62` 和 `LK-RETURN: restored action=0`。
- action=1 分步返回原厂 LK 后继续启动，以及 direct3 单次 RAM 引导均已进入 Android A：`sys.boot_completed=1`、ADB/root、文件级加密状态确认。无需额外重启；A/B固件和boot分区哈希保持。
- 正常菜单从 ESP 加载本应用、以及 B 闪存常驻固件普通重启后的同一路径均已启动 Android A。B 镜像为 `072bcff0…`，完整读回一致；A、boot_a、boot_b 保持原值。用户已验收实体按键选择与启动。
- 2026-11-11 的 128 MiB 版本修复两块内存的缓存清理与 LK 返回后的看门狗时限。完整菜单 RAMBoot、B 常驻普通重启均已启动 Android A；后者完成启动后继续观察约 192 秒，无复位。具体镜像与验证边界见组合层 `VALIDATION.md`。

## Experimental（实验中）

普通动作请求返回 LK 后选择 Android A，仅修改本次 LK 内存中的分区后缀，持久选择仍为 B。普通重启的常驻路径已验证，彻底断电启动仍待验收；旧的常驻固件不含返回描述符，应用会返回 `EFI_UNSUPPORTED`。

平台只支持已匹配的 EL2、单活动启动 CPU、原厂 LK 和固定内存布局。EFI 不访问 LK 上下文；由恢复 stub 在关闭 EFI MMU 后读取。退出 UEFI 后若恢复失败，保留 PMIC 看门狗恢复路径。

交接通过平台公开的 `W202dsMemory.h` 清理两块 UEFI 内存（56 MiB + 72 MiB），然后关闭缓存和 MMU。应用本身也可能分配在第二块内存中；遗漏该区域会在关闭缓存后执行到尚未写回的指令。Linux 与 Android 内核交接还清理 OS 装载区。

配套 LK 返回桥在继续启动 Android 前重新设置 180 秒恢复看门狗。Android 不再继承已被菜单和装载过程消耗的 RAMBoot 30 秒时限；EFI 应用本身不直接调用原厂看门狗接口。更新这项行为需要同时更新 LK 桥所在的固件，仅替换 ESP 上的 EFI 文件不足以修复超时复位。

## 构建输入输出

`efi/PocketJsUefiPkg.dsc` 独立构建输出 `AndroidLk.efi`，组合构建将其安装到 ESP 的 `EFI/Android/bootaa64.efi`。平台 FV 不再包含本应用；诊断时从 ESP 显式启动，不再由平台宏嵌入 FV。本应用不处理 Linux EFI 项目。

LK 侧通过显式 `HANDOFF_INCLUDE` 消费 平台的 `Ums9620Pkg/W202ds/Include/LkReturn.h`，以 `LK_RETURN=1` 构建匹配的 RAMBoot loader。上下文为 `0x86400000`，描述符为预留页内 `0x82ffe200`，返回 stub 位于 `0x86000000` 下载模块的前64 KiB。

`tests/test_handoff_cache.py` 将三个交接入口分别放到两块内存中执行，核对汇编实际清理范围与平台 MMU 映射、关闭缓存的顺序及跳转参数。它依赖 AArch64 GCC/binutils 和 Unicorn；输入由 `PJB_EDK2_ROOT`、`W202_PLATFORM_ROOT`、`W202_TEST_OUTPUT` 指定。仿真不模拟真实缓存一致性。

## TODO

- 彻底断电后的启动验收。此前菜单退回为 BDS 缺失 EndOfDxe 导致的 `EFI_ACCESS_DENIED`，已修复并通过常驻普通重启测试。
- 多次往返、长时间菜单停留以及设备重新初始化测试。

## 来源

EFI 应用与返回 ABI 为本项目实现；原厂 ABI 由本机 LK 备份及既有 RAM 探针确认。高通 ABL 作为 EFI Android loader 的架构可供参考，但不是本程序的二进制或硬件实现来源。
