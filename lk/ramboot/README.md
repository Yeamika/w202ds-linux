# W202DS LK RAMBoot

## 简介

匹配 W202DS 原厂派生 LK 的 RAMBoot、探针和 UEFI 常驻 bootstrap。共用 `../uefi_ramboot.[ch]` 的镜像解析器；完整原生 W202DS LK 尚未完成。

```text
ramboot/
├── loader/              # enter、resident 与 ARM64 bootstrap
├── probes/              # CPU、镜像、看门狗、保留区与交接探针
├── tests/               # 基于真实 ARM64 二进制的仿真
├── Makefile
├── pack-check.py        # PLK1 与 Android wrapper 组合
└── package-resident.py  # UEFI 常驻 5 MiB 容器
```

## 实现与历史验证

- PLK1 RAM 模块可读取 CPU/页表状态、检查镜像、测试看门狗及保留交接页。
- 镜像长度、CRC、FV、TE 和 SEC 入口检查；2 MiB FD 默认加载到 `0x82800000`。省略的 `0xff` 尾部在加载时恢复，不新增压缩编码。
- 已有实机 RAMBoot 进入 SEC／PEI／DXE／PocketJS 和 EFI 子程序返回记录；r61 有常驻冷启动证据。
- `loader/resident.c` 只截普通／外部复位，其余模式保留原厂路径；失败转入原生 Fastboot。
- 先停止旧看门狗，再在面板初始化前启用恢复看门狗；冷启动调用原厂 `lcd_enable` 建立扫描层，开启 EIC164 音量上数据输入。
- 封装器生成精确 5 MiB 容器，保留匹配的签名副本、证书和两个 PJB 设置 bank；新候选须独立验收。

## Experimental（实验中）

`LK_RETURN=1` 可保存 CPU、GIC 和计时器上下文，供 EFI 适配程序退出后返回同一次启动的 LK。2026-11-05 RAM 往返 probe、正常菜单及 B 常驻普通重启后经原厂 LK 继续启动 Android A 均已通过，Android完成启动、ADB/root正常；用户已验收实体按键启动。6项 ARM64 仿真覆盖寄存器／失败分支、下载区权限、Fastboot应答及冷启动入口；仿真不建模真实GIC硬件。当时的5 MiB容器 `072bcff0…` 已写 B 并完整读回。此前菜单退回的 EFI 安全通知问题已在外层修复；彻底断电和反复启动仍待验收。底层 Makefile 默认未启用，仓库根 build.py 显式启用；该构建另需 `HANDOFF_INCLUDE/LkReturn.h`。

原生引导前必须离开 `0x86000000` 的 RAM 模块，再用已匹配的 LK helper 恢复下载区 RW+XN；原厂加载器会用这个区域装入 boot 镜像。Fastboot路径保留原命令应答，冷启动路径直接续接 normal_mode。`W202_LK_RETURN_DEFER_ANDROID` 是只用于分步诊断的构建开关：选好 Android 后返回 Fastboot，随后由宿主发送 continue；不用于交付镜像。

当前借用原厂 DDR、时钟和面板初始化，只接受 EL2／单活动 CPU；未实现本板 SMP。RAM、缓存、异常及完整次级 CPU 交接仍需审计。常驻启动通过不等于触控、页面流畅度或 EFI 系统加载器通过。

诊断构建使用 `W202_RESIDENT_PREFLIGHT=1`，故意不启用 DXE 续期；正常常驻与诊断镜像要分别构建，`--preflight` 只描述已选定的 loader。

RAM 入口现在与 resident 一样补齐 EIC164 数据输入掩码；恢复 LK 时还原原值。缺失此位会使整组按键协议不安装，曾导致音量与电源键同时失效。

## TODO

- 继续完善 RAMBoot、交接与恢复，并做多次冷启动稳定性验收。
- 接入完整原生 W202DS target，逐步替换固定地址调用。
- 真机验收按键、存储及 EFI OS 加载；不能以菜单项目存在视为系统已可启动。

## 构建

在 w202ds-boot 仓库根目录执行，输出目录须在源码之外：

```sh
make -C lk/ramboot \
  CROSS_COMPILE=/path/to/bin/aarch64-linux-gnu- \
  OUT=/path/to/project/output/ramboot \
  HANDOFF_INCLUDE=/path/to/public-loader-abi \
  EXTRA_CFLAGS=-DW202_RECOVERY_WATCHDOG_MS=180000 \
  all check-loader watchdog-probe retention-probe handoff-probe enter-sec resident
```

`HANDOFF_INCLUDE` 由外层提供 `LoaderHandoff.h`；纯只读探针不需要它。`package-resident.py prepare` 产生布局与 bundle，再构建 `resident-bootstrap`，最后 `package`。参数见 `--help`。工具只处理文件，刷写由外层负责。

`tests/test_*.py` 通过环境参数接受真实镜像、ELF 和 FD，使用 Unicorn 执行 ARM64。仿真不代替真机；本次新构建的验证范围见仓库根目录 VALIDATION.md。

## 来源与参考

解析器、公开上游和许可证见 [仓库说明](../../README.md) 与 [输入版本](../../inputs.lock.json)。固定原厂 ABI、签名基线和冷启动日志来自本机备份与探针；实际文件选择由外层传入。本目录与旧 PJB 的 `resident/resident-*.S` 分别维护各自的启动逻辑。
