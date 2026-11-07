# W202DS USB-C DisplayPort 外接显示（w202_dp.ko）

生成 `w202_dp.ko`，驱动 USB-C DP Alt Mode 外接显示：USB3 与 2 lane DP 同时工作（pin D，TCA mux 3），最高 HBR2，单个外接显示器。内屏 `w202_r6p0` 不受影响，本模块注册独立的 KMS-only DRM 设备 `w202-dp`，不提供 fbdev。

链路：DPU1（0x31880000，dpu-lite-r3p0）→ DesignWare DPTX（0x31890000，树内 `dw-dp`）→ USB3.1/DP combo PHY（ssphy，树内）→ redriver → USB-C。

```text
w202-dp/
├── Makefile
├── w202_dp.h            # 私有寄存器、状态、锁与电源用户
├── w202_dp_main.c       # sprd,dptx 平台驱动、PD_DPU_DP 电源、dw-dp 平台数据、HPD、末级 bridge、码流恢复
├── w202_dp_dpu1.c       # DPU1 CRTC / primary plane、像素时钟、vblank 与翻页完成
├── w202_dp_typec.c      # w202-usbc-mux 上的 typec switch / mux（仅 pin D）
├── w202_dp_redriver.c   # PI3DPX1217 / PTN38003A SMBus 设置与 GPIO120 方向
└── README.md
```

## 依赖

本模块依赖内核子模块的以下改动（见 kernel 分支 `usb-single-cable`），不能对未打补丁的内核编译或加载：

- `CONFIG_DRM_DW_DP=m`，含 `dw_dp_plat_data` 扩展、`dw_dp_hw_reinit()` 与 `dw_dp_restore_stream()`；
- `CONFIG_GENERIC_PHY=y`、内建 `CONFIG_USB_SPRD_UMS9620_SSPHY=y`，导出 `devm_sprd_ssphy_get_dp_phy()`、`sprd_ssphy_set_orientation()`、`sprd_ssphy_set_dp_ops()`（头文件 `include/linux/phy/phy-sprd-ums9620.h`）；
- `CONFIG_TYPEC_DP_ALTMODE=m`、`CONFIG_TYPEC_UMP9620=m`（`ump9620-tcpc.ko`，注册软件节点 `w202-usbc-port` / `w202-usbc-mux` / `w202-usbc-dp`）。

模块依赖 `dw-dp`、`drm_display_helper`（bridge connector）、`typec`；软依赖先加载 `ump9620_tcpc` 和 `typec_displayport`。设备树沿用 `../../devicetree/w202ds-usb3.dtsi` 已有的 DPU1、DPTX、graph 与 `dp-gpios`（GPIO120），无需新增节点。

## 构建输入与输出

```sh
make -C Devices KDIR=/匹配内核输出 OUT=/临时目录 w202ds-usb3-dp
# 或直接：
make -C /匹配内核输出 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M="$PWD" modules
```

输入为本目录源码、已完成 `Image modules` 的匹配内核输出（全局 `Module.symvers` 提供 dw-dp / ssphy / typec 符号）和 AArch64 工具链；输出一个 `w202_dp.ko`。Makefile 带 `-Werror`。安装位置与其他外部模块一致：`updates/w202ds/`。

## 加载顺序与 probe

1. `ump9620_tcpc` 在 `module_init` 注册软件节点；本模块 probe 在节点出现前延迟。
2. 本模块 probe 依次：取得 syscon、DPTX/DPU1 映射、时钟、GPIO120、DP PHY（ssphy 未绑定时延迟）、DPU1 时钟与 IRQ；打开 PD_DPU_DP；`dw_dp_bind`；建立 DRM 管线；关闭 PD_DPU_DP；注册 `w202-dp` 卡；**最后**注册 typec switch / mux。
3. 此后 `ump9620_tcpc` probe 不再延迟（`typec_register_port()` 需要 mux 与 switch）。

卸载时先卸 `ump9620_tcpc` 再卸 `w202_dp`：端口仍持有的 typec mux 不能安全拆除。

## 结构

| 部分 | 文件 | 说明 |
| --- | --- | --- |
| `sprd,dptx` 平台驱动 | `w202_dp_main.c` | 通过 OF graph（dptx `port@1` → dpu1）找到 DPU1；dpu1 设备保持无驱动，用 `of_graph_get_remote_node`、`of_address_to_resource`、`devm_get_clk_from_child`、`irq_of_parse_and_map` 取资源。 |
| PD_DPU_DP 电源 | `w202_dp_main.c` | 用户位掩码：probe、HPD、CRTC、bridge、force、TCA。任一用户存在时上电；`hw_get`/`hw_put` 给 dw-dp 嵌套访问，掉电时寄存器访问返回 `-ENODEV`；掉电前最多等 5 s 用户退出。 |
| DPU1 CRTC / plane | `w202_dp_dpu1.c` | GEM-DMA（CMA），XRGB8888/ARGB8888，仅 layer 0、无缩放。vblank 来自 INT VSYNC；翻页经 REG_UPDATE 锁存、UPDATE_DONE 完成。 |
| dw-dp bridge | 树内 `dw-dp` | `plat_data`：HBR2（540000）、single pixel、ssphy 提供的 PHY、`ctrl_phy_rate_eq`、`hpd_status_bit8`、`optional_clks_rst`、`full_reset`、`aux_poll`（参数）、`post_reset`、`hw_get`/`hw_put`。 |
| 末级 bridge（`w202_guard`） | `w202_dp_main.c` | HPD 为低时 detect / EDID 返回断开 / NULL。总线格式固定 RGB888_1X24（8 bpc RGB）。`pre_enable` 到 `post_disable` 持有电源；原厂同步极性模式下改写 DPTX 0x30c；DPTX 视频开启后再运行 DPU1，DPTX 关闭前先停 DPU1（`sprd_dp.c`）。 |
| connector | `drm_bridge_connector` | `connector->fwnode` = `w202-usbc-dp`，`typec_displayport` 的 `drm_connector_oob_hotplug_event()` 由此送达。 |
| typec switch / mux | `w202_dp_typec.c` | 都注册在 `w202-usbc-mux`。pin D：HPD 高 → 插入、IRQ_HPD → 脉冲、HPD 低 → 拔出；SAFE/USB → 拔出。pin C/E 返回 `-EOPNOTSUPP` 并记录。方向变化 → redriver 方向、GPIO120 及 TCA 方向事务。 |
| redriver | `w202_dp_redriver.c` | 对现有无驱动 i2c client 2-0052、6-0030 做 SMBus 访问；仅上电后访问，ID 寄存器 NACK 视为不存在，直到下次上电。 |
| 码流恢复 | `w202_dp_main.c` | HPD 重新插入且输出仍点亮时，延迟 `restore_delay_ms` 检查并用 `dw_dp_restore_stream()` 重新启动已停止的 DPTX 视频流。 |

## 寄存器流程与来源

来源缩写：`V/` = 原厂 5.15 内核 DP 源码（`sprd/dp`、`qogirn6pro/global_dp*.c`、`dpu_lite_r3p0.c`）；行号指该原厂树，`../vendor-reference/drivers/gpu/drm/sprd/` 有同名文件可对照，但行号不一定一致。`KO` = 本机原厂 `sprd-drm.ko` 反汇编（地址为模块内偏移）。

### 上电（`w202_dp_hw_power_up`；KO `dp_detect` @0xe23c、`sprd_dp_pd_event` @0x9a0c）

| # | 读写 | 来源 |
| --- | --- | --- |
| 0 | 轮询 PMU 0x548[12:8] == 0（PD_IPA 已开），10 µs 步进，100 ms | KO @0x9a0c |
| 1 | PMU 0x3e4 bit25（`PD_DPU_DP_FORCE_SHUTDOWN`）← 0 | `V/global_dp.c:146` |
| 2 | 轮询 PMU 0x548[20:16] == 0，100 ms；超时则 bit25 ← 1 并失败。**此前不访问 0x3188/0x3189。** | KO |
| 3 | `clk_bulk_prepare_enable`：`clk_ipa_apb_dpu1_eb`、`dptx_eb`、`tca_eb`、`usb31pll_eb` | `V/global_dp.c:119-130` |
| 4 | dispc1_glb 0xb0 = 0x1（旁路 HDCP） | `V/global_dp.c:151-152` |
| 5 | usb31_dp 0x5c \|= 0x400（AUX PHY PWDNB） | `V/global_dp.c:155-157` |
| 6 | DPU1 0x1f0 = 0x3（DPI 极性） | `V/global_dp.c:193` |
| 7 | `phy_power_on(dp phy)`：ssphy DP 引用，不复位 USB3 | KO 步骤 7 |
| 8 | `dw_dp_hw_reinit()`：SOFT_RESET 0x1d、CCTL、HPD/AUX IEN；随后 `post_reset`：DPTX 0xc08 = 0x5，IEN 0xd04 &= ~BIT7。绑定时由 `dw_dp_bind` 完成 | KO `dptx_core_init` @0x3a274 |
| 9 | `enable_irq(SPI 123)` | – |

### 插入（`w202_dp_hpd_event(PLUG)`）

1. 取得 HPD 电源用户（掉电时执行上述上电）。
2. 方向（KO `dp_detect` 步骤 8，@0xe434-0xe4bc）：`dir` 取 typec 方向（NORMAL 0 / REVERSE 1），没有时取 AON 0x2c bit10（置位 → 0），二者不一致时告警；`redriver_dir` 参数可覆盖。
   - PI3DPX1217 若应答 reg 0x00：03 ← 0x60 \| dir<<4，04 ← 0x06，05/06/07 ← 0x7c，然后同原厂再写一次 07 ← 0x7c（`pi3dpx_reg08=1` 时改写 08）；来源 `pidpx1217-i2c.c` mode 3。
   - PTN38003A 若应答 reg 0x00：04[7,2:0] ← 0x82，06[3:0] ← 0x0a（2 lane、HBR2）；来源 `ptn38003a-i2c.c`。
   - GPIO120（`dp-gpios[1]`）：PI3DPX1217 存在时 ← 0，否则 ← `dir`。
3. HPD 注入：dispc1_glb 0x50 = 0xa，再 0xe（`V/global_dp.c:221-232`）。
4. 20 ms 后 `drm_bridge_hpd_notify(connected)`。

IRQ_HPD：dispc1_glb 0x50 = 0xb，再 0xf（`V/global_dp.c:233-240`）。原厂忽略 IRQ_HPD，`forward_irq_hpd=0` 可与其一致。

### 拔出与掉电

1. dispc1_glb 0x50 = 0x8，再 0xc（`V/global_dp.c:226-231`）。
2. redriver 恢复默认：PI3DPX1217 03 ← 0xc0、04 ← 0x04、05/06/07 ← 0x08；PTN38003A 06[3:0] ← 0。
3. 释放 HPD 用户。没有用户且 `idle_shutdown=1` 时：等待 `hw_users` 归零 → `disable_irq(SPI 123)` → DPTX IEN &= ~BIT7、ISTS = 0xff、HPDSTS = 0x7 → `phy_power_off` → 关时钟 → PMU 0x3e4 bit25 ← 1（`V/global_dpu1.c:334-348`）。

### DPU1 CRTC

- 时钟（`global_dpu1.c:150-245`）：`dispc1_eb` 开；`dpu_core` 父时钟 512 MHz；`dpu_dpi` 父时钟 pixelpll；pixelpll = pclk × min(297 MHz / pclk, 16) × 9；`dpu_dpi` = pclk × 9。
- 初始化（`dpu_init`）：BG_COLOR (0x2c) = 0；PANEL_SIZE (0x20) = v<<16 \| h；CFG0 (0x08) = 0；CFG1 (0x0c) = 0xa<<12 \| 0xa<<8 \| 0xd<<4 \| 0xc \| BIT22-24；LAY_CTRL[0-7] = 0；INT_CLR (0x1e4) = 0xffff；SDP_CFG (0x420) = 0；CURSOR_EN (0x400) = 0；**MMU_EN (0x804) = 0**（物理地址扫描）。
- DPI（`dpu_dpi_init`）：DPI_CTRL (0x1f0) = 3（原厂）或 `sync_pol_vendor=0` 时取模式标志；H/V_TIMING (0x1f4/0x1f8) = sync \| bp<<8 \| fp<<20；INT_EN = DONE \| ERR \| UPDATE_DONE（vblank 开启时加 VSYNC）。
- Layer 0（`dpu_layer`）：ADDR (0x30) DMA 地址（< 4 GiB，16 字节对齐）、SIZE (0x44)、CROP (0x5c)、ALPHA (0x50) = 0xff、PITCH (0x48) = pitch / 4、CTRL (0x40) = EN \| ARGB8888 \| layer alpha。
- 运行：末级 bridge enable 时在 DPTX 视频后置 CTRL RUN；翻页置 REG_UPDATE，UPDATE_DONE 时发送事件；停止置 STOP 后等待 INT DONE 500 ms。
- 同步极性（`sync_pol_vendor=1`，默认）：DPU1 恒为高有效；原厂 DPTX 对**负**极性设置 VSAMPLE_POLARITY 0x30c bit1/bit0（`V/dp/dp_video.c:253-261`），主线 dw-dp 对正极性设置，末级 bridge 在 dw-dp 开视频后、DPU1 RUN 前改写 0x30c。

### 模式过滤

拒绝隔行与 doublescan；hdisplay / vdisplay 大于 4096；sync 大于 255、porch 大于 4095；pclk 大于 297 MHz；24 × pclk 超过 HBR2 × 2 lane 有效带宽（8.64 Gbit/s）；pixelpll 低于 800 MHz 或时钟框架无法在 0.5 % 内取到。dw-dp 另做链路带宽检查。PD_DPU_DP 未上电（无 HPD、无 `force_power`）时，末级 bridge 的 `atomic_check` 拒绝点亮输出的 modeset。

## 参数与诊断

模块参数（`/sys/module/w202_dp/parameters/`）：

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `aux_poll`（0444） | Y | 轮询 AUX 应答，AUX_REPLY 中断保持关闭（原厂 IEN = 0x95）；绑定时生效。 |
| `sync_pol_vendor`（0644） | 1 | 1 = 原厂同步极性处理；0 = DPI_CTRL 取模式标志并使用 dw-dp 自己的 0x30c。下次 modeset 生效。 |
| `forward_irq_hpd`（0644） | Y | 注入 IRQ_HPD 脉冲。 |
| `pi3dpx_reg08`（0644） | N | 第 4 个 EQ 值写 reg 0x08，而非原厂重复写 0x07。 |
| `redriver_dir`（0644） | -1 | -1 自动（typec，否则 AON bit10），0 正向，1 反向；下次插入或方向变化生效。 |
| `idle_shutdown`（0644） | Y | 空闲时关闭 PD_DPU_DP；N 首次使用后保持上电。 |
| `restore_stream`（0644） | Y | HPD 重新插入且输出仍点亮时，重新启动已停止的 DPTX 视频流。 |
| `restore_delay_ms`（0644） | 1000 | HPD 插入到码流检查的延迟（ms）。 |

设备属性（`/sys/bus/platform/devices/31890000.dptx/`）：`force_power`（rw，1 保持电源、时钟与 DP PHY，仅调试）、`hpd`（rw，写 `plug` / `unplug` / `irq` 手动注入 HPD）、`hw_id`（ro，PMU 状态始终可读，DPTX 身份寄存器仅上电时可读）、`status`（ro，电源用户、HPD、typec 方向与 AON bit10、实际方向、DPU1 状态、redriver 是否存在）。

调试：`echo 'module w202_dp +p' > /sys/kernel/debug/dynamic_debug/control`，以及 `drm.debug=0x106`。

## 桌面集成

`../../userspace/integration/udev/rules.d/72-w202-mutter.rules` 让 GNOME Mutter 保持内屏 `w202-r6p0-lk-dsi` 为主 KMS 设备，并保证 `w202-dp` 卡不被忽略、不成为主 GPU（作为副 GPU，CPU 复制到 GEM-DMA dumb buffer）。KDE 未验证。

## 风险与待确认硬件问题

1. **PD_DPU_DP 控制**：本驱动首次写 PMU 0x3e4 bit25；0x548[20:16] 不归零时 probe 干净失败并恢复 bit25。ssphy USB 初始化会写 DPTX 0xc08 bit0，若 USB3 依赖 DP 域上电，可设 `idle_shutdown=0`。
2. **AUX**：默认轮询；AUX_REPLY 是否送达 SPI 123 未确认。
3. **像素时钟**：pixelpll = pclk × N × 9、dpi = pclk × 9 照搬原厂公式；pixelpll 也是 `pixelpll-668m25` / `pixelpll-297m` 的父时钟，改频会影响二者（目前无使用者）。
4. **MMU_EN**：0x804 复位值未知，写 0 走物理地址扫描，缓冲区必须是 4 GiB 以下的 CMA；plane check 拒绝其他缓冲区。内屏与外屏共享 CMA，1080p XRGB 缓冲约 8.3 MB。
5. **redriver**：本机两颗芯片的 ID 读取均 NACK（`-5`），供电/使能未知；实际方向靠 GPIO120 回退。PTN38003A 速率在插入时固定写 HBR2/2 lane，链路降速时不跟随。
6. **方向来源**：原厂用 AON 0x2c bit10，本驱动优先 TCPM 并在不一致时告警。
7. **同步极性**：负极性模式（如 CVT 1440x900）未验证。
8. **卸载**：先卸 `ump9620_tcpc`；主线 dw-dp 解绑后挂起的 `hpd_work` 由 `hw_get` 拦截但不取消。
9. **pin C/E**（4 lane，TCA mux 2）不支持；默认端口 VDO（0x846）只提供 pin D。

## 来源

原厂 UMS9620 / qogirn6pro 5.15 DP / DPU1 源码与本机原厂 `sprd-drm.ko`、`pidpx1217-i2c.c`、`ptn38003a-i2c.c`；主线 `drivers/gpu/drm/bridge/synopsys/dw-dp.c`。GPL-2.0。
