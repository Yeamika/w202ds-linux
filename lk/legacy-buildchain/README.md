# 遗留 LK 构建链

## 待办

- [x] lk-debuger Fastboot 完整刷写：原生 `flash:` 入口替换为显式 GPT `_a` / `_b` 全分区写入并读回比较；下载只进 RAM，不调用 sparse 流式写入或备份同步。
- [ ] 新刷写入口和 UEFI 并行 USB 服务的实机验证；构建清单仍为 `hardware_tested: false`。

## 最小 LK

```sh
make -C lk/legacy-buildchain OUT=/path/to/fresh/lk-build
```

依赖 Python 3.9+ 和 AArch64 GNU 工具链；可设置 `CROSS_COMPILE`。输出目录须在仓库外且不存在。
输出 `lk-debuger.img`（5 MiB）、`manifest.json`、两个 PLK1 测试模块、`ramboot-memory.bin` 只读内存工具和 `ramboot-screen.bin` 显示探针。
它从 `lk/firmware/uboot-mod.bin` 重建 RAMBoot 基础，只复用原生 LK/Fastboot/USB，不包含 EDK2、PocketJS 或完整的 UEFI 返回上下文。

默认正常上电、外部复位进入原生 Fastboot。执行 RAM 模块前启动 30 秒看门狗，设置 Fastboot 复位原因；模块返回后停止计数并恢复原值。模块卡死时由硬件复位回 Fastboot，电脑端重新连接。Fastboot 空闲等待和下载时不启动这只看门狗。
充电、恢复、低电保护等其他启动模式保留原生处理。

调试版复用原生 `lcd_puts()`、光标定位、字体和同步接口，在原 Fastboot 文字下显示就绪、RAM 下载、模块执行、返回或拒绝状态。等待模块返回时保留执行提示；启用看门狗时标明已启动超时保护。屏幕未初始化则跳过，关闭 `SCREEN` 后不接入显示补丁。这些提示在 LK 中运行，进入 UEFI 后的详细进度由 UEFI 自己提供。

| 编译选项 | 默认 | 功能 |
| --- | --- | --- |
| `DEBUG` | `1` | 下列三个功能的默认值；`0` 关闭调试恢复和屏幕提示 |
| `WATCHDOG` | `$(DEBUG)` | 执行 RAM 模块前启动看门狗 |
| `USB_RECOVERY` | `$(DEBUG)` | 正常/外部复位直接进 Fastboot |
| `SCREEN` | `$(DEBUG)` | 通过原生 LCD 控制台显示调试动作与结果 |
| `FASTBOOT_FLASH` | `1` | 整分区 raw 下载、显式槽写入和逐段读回；`0` 保留原生 flash/download |
| `WATCHDOG_MS` | `30000` | 模块超时，1000–180000 毫秒 |

```sh
make -C lk/legacy-buildchain OUT=/path/to/fresh/production-lk DEBUG=0
# 也可以单独设置 WATCHDOG=0、USB_RECOVERY=0 或 SCREEN=0
python3 lk/legacy-buildchain/build.py --output /path/to/fresh/lk --no-debug
```

Android SDK `platform-tools` 的 Fastboot 可直接使用，Windows 换成 `fastboot.exe`：

完整刷写示例：`fastboot -s <serial> flash uboot_b /path/to/lk-debuger.img`。
镜像必须与目标 GPT 分区大小完全相同，尾部不足须由镜像制作方填充。
拒绝无槽名、ESP、sparse 镜像及部分分区写入；成功 `OKAY` 表示完整读回比较通过。
不会修改另一个槽。`erase`、`set_active` 等其他原生命令不在这个替换入口内。

```sh
fastboot -s <serial> stage /path/to/lk-build/ramboot-smoke.bin
fastboot -s <serial> oem jump-rmk :probe

# 只在 RAM 验证原生 LCD 显示接口，结束后继续 Fastboot
fastboot -s <serial> stage /path/to/lk-build/ramboot-screen.bin
fastboot -s <serial> oem jump-rmk :probe

# 故意停住，验证看门狗复位后 USB 能再次发现 Fastboot
fastboot -s <serial> stage /path/to/lk-build/ramboot-hang.bin
fastboot -s <serial> oem jump-rmk :init
```

`stage` 只下载到 RAM。入口接受 PLK1 模块，`:probe` / `:init` 的含义由模块决定；省略操作默认 `:probe`。原有直接 `jump-rmk` 协议仍兼容，其他 OEM 命令继续交给原生 LK。调试关闭后保留这两个 RAMBoot 入口。

通过同一 Fastboot 接口读取 RAM，不写存储。默认读取 `0x82ffe000` 起的 8 KiB，包含 UEFI 交接页和 RAM trace：

```sh
python3 lk/legacy-buildchain/debug/read.py --serial <serial> \
  --module /path/to/lk-build/ramboot-memory.bin --output /path/to/new/ram-read
```

可用 `--fastboot` 指定 SDK 可执行文件，`--address`、`--size` 指定 16 字节对齐的 DDR 范围，每次最多 64 KiB。支持原有 `0x80000000..0xc0000000` 和 UEFI 额外内存区 `0xc8000000..0xcc800000`；两段之间的空隙与跨边界请求会被拒绝。工具验证每段地址和整体 CRC32，输出 `memory.bin`、`metadata.json`、原始 Fastboot 记录，以及存在有效 trace 时的 `trace.txt`。不会通过 SPD 加载器重新初始化 DRAM；复位后原 UEFI 内存可能已被复用，读取成功不代表异常现场仍完整。

其他固件可把此镜像作为受校验的 LK 输入。UEFI 组合构建默认自动生成关闭调试恢复的最小 LK；加 `--lk-debug` 为自动生成的基础启用调试，也可用 `--lk-baseline`（或 inputs 中的 `lk_baseline`）传入已构建镜像。封装器校验扩展和所有其他字节，随后接入自己的 resident。启动痕迹只保存在 RAM；没有 eMMC 日志 bank 或选择记录。

UEFI 组合构建强制基础 `SCREEN=0`，用 `ramboot/Makefile` 的 `LK_RETURN=1 UEFI_FASTBOOT=1`
加入可返回的轮询服务。该路径只复用固定 LK 的 DWC3、raw 分区 I/O；每次调用保存并恢复
UEFI 地址空间和寄存器，临时屏蔽 LK 的打印/LCD 入口，保持 UEFI 显示所有权。
下载使用独立保留的 128 MiB RAM，上限内的完整 raw `_a/_b` 分区镜像可以刷入。
协议、真实 SDK 客户端、故障注入和 ARM64 指令仿真测试位于 `fastboot/`；硬件缓存、
USB 时序、拔插和实际分区写入仍以实机验证为准。原生驱动初始化和存储 I/O 可能有短暂延迟。

加入屏幕提示前的 30 秒调试版已在 W202DS 使用 Android SDK Fastboot 34.0.5 实测：PLK1 模块正常返回，故意卡死约 30.2 秒后复位，约 34.3 秒重新出现 Fastboot；随后空闲观察 45.9 秒，命令仍正常响应。该版本镜像 SHA-256：`051af9acf6d08898dca1772a37385c3a479644dfa65399de29c8130f17386873`。原生 LCD 接口另经 RAM 探针验证，读回帧缓冲可见文字，Fastboot 继续响应。

以上整镜像验证覆盖加入屏幕提示前的最小 PLK1 模块。包含屏幕提示的新整镜像及其他开关组合仍需实机验证，manifest 的 `hardware_tested` 默认保持 false。RAM 模块进入后若主动停用看门狗，基础 LK 无法继续提供该超时保护。

```text
build.py        最小 LK 构建入口；Makefile 提供编译开关
debug/          小型 RAMBoot/恢复补丁、格式校验和 ARM64 仿真
fastboot/       完整刷写、无显示轮询服务及 SDK/故障注入/ARM64 测试
ramlk/           从 mod 补零、编译 hook、生成 RAMLK
ramboot/
  bridge/        共用的 RAMBoot 镜像解析与桥接代码
  loader/        UEFI 进入、常驻和返回代码
  probes/        探针
  tests/         桥接测试
  Makefile       加载器构建入口
  package-resident.py  UEFI 分区封装
```

输入为 `../firmware/uboot-mod.bin`。设置由运行时保存在 ESP 文件中，不再植入旧 bank。`ramlk/build.py` 保留历史 RAMLK 的精确重建；本目录的 `build.py` 在其上构建最小 LK，`ramboot/` 可继续合入 UEFI。组合入口为仓库根目录的 `pocketjs4edk2_uefi/build-firmware.py`。
