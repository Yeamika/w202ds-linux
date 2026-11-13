# 本机 AUDCP / VBC 协议依据

本实现针对 W202DS（UMS9620 / Qogirn6pro）。REA 分析对象是本机原厂
`sprd_audcp_boot.ko`、`sprd-vbc-codec.ko`、SIPC / DSP access 模块，以及
`vendor_a/lib/hw/audio.primary.whale.so`。HAL 为 32 位，SHA256 为
`c61ef393b642b033100bdf7c8d0fc7dfc5ce1df9966a7a55ec1752a9584fa5a8`。
原厂路由及参数来自本机 ODM XML；这些二进制与 XML 不随新源码发布。

## 内存与邮箱

| 内容 | 物理位置 / 大小 |
| --- | --- |
| 固件 reservation | 0xafa00000 / 0x600000 |
| 既有 audio reservation | 0xaf700000 / 0x300000 |
| 参数共享区 | 0xaf980000 / 0x400 |
| TX / RX 环及索引 | 0xaf980400 / 0xa10 |
| profile 共享区 | 0xaf981210 / 0x1400 |

不创建重叠 reservation。IPC 仅映射上述必要区域，不能清零整块 3 MiB audio memory。
TX、RX 各 64 条 20 字节消息，结构是 `le16 command, le16 channel, le32 parameter[4]`。
尾部四个 u32 是单调读写索引。doorbell 为
`command << 48 | channel << 32 | parameter[0]`，mailbox 使用目标 core 5 / sensor 0。
启动通知 command/channel/parameter0 均为 0xbeee；唤醒 doorbell 为 100。
channel 2 / command 0x25 表示 DSP assertion。

启动向量为 `(firmware_phys + 0x80) >> 1`，必须保留原固件 0x80 头部。
`sprd_audcp_boot.ko:start_store` 的 DDR mode 0 提供 reset、bootprotect 和 vector 顺序。

释放 reset 后 AGCP 仍处于掉电：PMU 0x544 的 AGCP SYS（bits 12:8）与 DSP（bits 20:16）
状态均为 7，DSP 不会运行，也不会发送 0xbeee。原厂 `agdsp_access_enable` /
`restore_access` 先发 doorbell 100，再轮询 `pwr_status4` 到 0（失败日志为
`wait agdsp power up timeout`）；`agdsp_can_access` 也以两个状态为 0 判定可访问。
因此顺序必须是：释放 reset → doorbell 100 → 等 SYS/DSP 状态为 0 → 等 0xbeee。
若先等 0xbeee，会稳定超时（-110），RX 环保持 0/0。实测按此顺序 48 ms 内收到启动通知。

AGCP 时钟门控 `56200000` 依赖 AUDCP power domain。DSP 模块晚于内核默认 10 秒
deferred probe 超时加载，门控会被移出 deferred 列表且不再重试；启动服务需在 DSP
绑定后显式 bind `ums9620-clk`（只 bind，不 unbind）。

启动时 DT 改写在内核 BSS 副本中进行。FDT fixmap 必须继续映射 LK 原始物理地址：
若把 fixmap 重映射到副本，arm64 `pgattr_change_is_safe` 拒绝改变 live PTE 的 PFN，
在控制台起来前 BUG，表现为停在开机 Logo、无 USB。

## 播放与调音

- command 4 / 5 为 startup / shutdown，startup 为 0x26c 字节，结构大小编译断言。
- command 7 为 trigger，parameter2=1。normal AP01 不发送 command 6 hw_params。
- command 9 是 control：master width ID 0x26，payload 单 u32；数字增益 ID 3，
  payload 为 `{startup_reload, id, left, right}`。
- command 11 是 profile，外层共享参数为 `{selector, type=1, physical, size}`，
  消息 ID=selector、stream=-1。DSP VBC profile 长度为 0x6c4。
- HAL `set_vbc_param`（REA 0x4f744）计算 selector 为
  `(xml_mode << 24 | logical_param_id << 16) + dsp_case`。
- HAL `get_audio_param_mode`（REA 0x598b8）用 XML offset / struct size 得到 mode。
- HAL 0x881d0 静态表为 `4b0000007d100200`，ID 75 指向 0x2107d 的
  `Music\Handsfree\Playback`。本机 speaker XML mode=64，最终 selector **0x404b0000**。
  XML common 列表中的排序 68 不是此 logical ID。

AP01 VBC 位于 0x56510000，左右 FIFO 偏移为 0 / 4，config=0x20，
enable=0x2c bits8/9，clear=0x30 bit2，DMA=0x50 bits0/1；watermark 为 80 个样本。
SPRD DMA 接口沿用同版本上游 `sound/soc/sprd/sprd-pcm-dma.c` 的 linked-list 约定，
左右独立传输，每个 stereo period 仅通知一次。STOP 后同步收回回调，prepare 重建描述符。

实机 AP DMA 中断为 **GIC INTID 88（SPI 56，level-high）**。导入设备树与
W205DS-ESIM Android 13 / 5.4.276 参考包都写成 SPI 55；W202DS 实测中，
DMA 已搬运且 raw/masked 状态均为 0x3，但 SPI 55 无中断。播放时 GIC pending
指向 INTID 88，停止 DMA 后该 pending 位消失。启动设备树覆盖为 SPI 56 后，
周期中断、连续写入和 drain 均正常。不能用参考板的 IRQ 值代替本机验证。

PCM open 的 ALSA constraint API 以负数表示失败，正数可表示约束成功更新；
不能把 `snd_pcm_hw_constraint_integer()` 的正返回作为 open 提前结束条件。

协议没有 transaction ID。共享参数串行处理；任何发送失败、环损坏、assertion 或 ACK
超时都会让实例永久 failed，避免迟到 ACK 被下一条命令误认。恢复需要重新启动 DSP。

功放上游 ACF 解析器可使用本机容器；额外的 MULTIBIN 0x12 在原厂与上游均被忽略，
不应为了绕过解析而修改 ACF。共享 GPIO61 由一个保持开启的 genpd 管理，codec 不各自申请 reset。

邮箱发送完成使用匹配控制器的 `mbox_chan_txdone`，与原厂 `aud_smsg_send` 一致；
控制器 send_data 已将 u64 同步复制到硬件或内部 FIFO。DSP 命令 ACK 仍独立从 RX 环接收。
内置 audio clock 驱动不支持本轮测试中的 unbind/rebind，音频按启动服务加载，禁止热重绑时钟。

## 麦克风采集（capture）依据

REA 对象：`snd-soc-sprd-codec-ump9620.ko`（SHA256 8873637d…bb51b）、
`snd-soc-sprd-codec-ump9620-power.ko`（7063b0b0…e5c1）、`snd-soc-sprd-vbc-v4.ko`
（441e60ab…）。ODM `audio_route.xml`、`codec.xml`。

### 拓扑

ODM 定义三路模拟麦克：`main_mic`（0x80000004，MIC1→ADC_1，ADD0_DATA_MIC13，
ADC/ADC1 LRCLK invert）、`aux_mic1`（0x90000000，MIC2→ADC_2，MIC12）、
`aux_mic2`（0x80000080，MIC3→ADC_3，MIC13）。普通录音 `only_codec_c`：
VBC ADC0/1/2 来自 IIS1/IIS2/IIS1，RX0–2 为 24 bit。DMIC 节点存在但 ODM 未用。
码流：UMP9620 模拟 ADC → ADD0 数据线 → AGCP 数字 codec（0x56360000）→
VBC ADC0（IIS1）→ DSP → AP01 录音 FIFO（VBC+0x8/0xc）→ AGCP DMA 请求 3/4。

### codec 寄存器空间（sprd_codec_read/write 0x125128/0x125318）

组件寄存器 0x2100–0x3290 → PMIC `reg - 0x3000 + 0x1000`（DT reg=0x1000）；
0x1000–0x12f4 → 数字 codec MMIO `0x56360000 + reg - 0x1000`，访问前需 AGCP
AHB reg0 bit21/22（`audcp-aud-eb`、`audif-ckg-auto-en`，set/clr 偏移 0x1000/0x2000）。

### 主麦 DAPM 通路（rodata widget/route 表）

`MIC Pin`←MICBIAS1；`ADC_1 Mixer`（MIC1PGA_1 Switch=0x3078 b1）→`ADC_1 Gain`
（0x3074 b9）→`ADC_1 Switch`（0x3074 b7，adc_1_event 0x126cb4）→`ADie Digital ADCL`
（0x2108 b3）→`Digital ADC In Sel`（0x1000[9:8]=ADC）→`Digital ADCL/R`（0x1000 b1/b3）
→`ADC`（Normal-Capture-AP01，chan_event）。原厂另有 NOPM 虚拟开关 `AUD ADC0L/R`
（HAL 选择 ADC 对，默认断开）；本驱动只有 ADC0 一对，直接连通，免用户态路由。供电：
ADC_1_CLK 0x3074 b5（subseq5）←BIAS←BG；CLK_ADC_Norm←CLK_ADC 0x3068 b9、
ADC_IBIAS 0x3074 b11←ADC_VREF_BUF b10（subseq3）；CLK_ADC←CLK_DIG_6M5 b11←
CLK_DCL_6M5 b12←CLK_DCL_32K←ANA_CLK b15←Analog Power；以及 DCL、
DIG_CLK_DRV_SOFT 0x3100 b1（audio_dcl_event 0x126418，POST_PMU：0x30fc|=0x8601、
0x3100|=0x400、lrdat-sel 时 0x305c|=0x300）、Digital Power（subseq1）。
ADC_1 Capture Volume=0x307c[5:3]（Music/Handsfree/Record 值 6）。
ADD0 DATA SEL 写 PMIC 0x234c mask 0x180，值表 0x119ae8 = {0,0x100,0x180}。

背麦 MIC3（ODM `aux_mic2` / `mainmic_auxmic2`）：`MIC3 Pin`←MICBIAS3；`ADC_3 Mixer`
（MIC3PGA_3 Switch=0x3078 b4）→`ADC_3 Gain`（0x3074 b12）→`ADC_3 Switch`（0x3074 b13，
adc_3_event 0x126fac：PRE_PMU 0x3078 b15 脉冲，0x3074 b15 清→4 µs→置位→4 µs；
POST_PMU b15 清，msleep 60）→同一 `ADie Digital ADCL`。ADC_3_CLK 0x3074 b14←BIAS；
ADC_3 Capture Volume=0x307c[8:6]（codec.xml 值 6）。ADD0=MIC13 且 LRCLK invert 时，
左声道=MIC1，右声道=MIC3；ADC_3 未上电时右声道恒为 -32768。

### 电源（power 模块 info 表，PMIC 地址 = en_reg - 0x2000）

VREG：PMIC 0x2008|=0x10，首次 0x2014 0x3100 置位 10 µs 后清（vreg_enable 0x104614）；
VB 0x1000 b13（sleep：b10）；BG b9；BIAS b4；MICBIAS1 0x1004 b0；MICBIAS3 0x1004 b2
（`SPRD_AUDIO_POWER_INFO_MICBIAS3`）；DCL 0x1100 b9；CLK_DCL_32K 0x1068 b13。
实测：VREG 关闭（0x2008 b4=0）时 PMIC 0x1000–0x1290 全部读回 0，DAPM 在声卡初始化时
读到 mixer 为断开，控件读回也为 0。驱动在组件存在期间保持 VREG 开启（原厂 VB 一次
standby 循环保留），默认值写入后可持续读回。
analog_power_event 0x125ff4 PRE_PMU：AON APB 0x350|=0x40（26M sinout），PMIC
0x200c|=0x83、0x2394|=0x2、0x2378|=0x100，0x2100|=2，0x2104 b1–2 脉冲 10 µs，
0x2140 b4 清，VB mode NORMAL；POST_PMD 反向（0x2378 保留）。
digital_power_event 0x125d80 PRE_PMU：0x100c b15 清、AD 采样率 0x1018[3:0]/[7:4]=48000/4000、
0x1010=0、LRCLK 0x1008 b2 / 0x1034 b0、0x1084=0x3303、0x1088 b0 清。
adc_1_event PRE_PMU：0x3074 b1 脉冲，b3 清→4 µs→置位→4 µs；POST_PMU：b3 清，msleep 60。

### VBC / DSP（vbc-v4）

scene_normal_startup 0x13e5fc：capture 仅 `dsp_startup(scene 0, stream 1)`（cmd 4）。
ap_hw_params 0x12bcec stream 1：0x24[23:16]=80、0x24[10:9]=3（L16），
vbc_phy_audply_set_src_mode 0x140ab0：0x28[7:4]=0、0x2c|=0xc。
scene_normal_trigger 0x13f598 start：0x30|=BIT3，0x2c|=0x3000，0x50|=0xc，
cmd 7 参数 {0, stream 1, 1}；stop 只清 0x50/0x2c。dsp_shutdown 0x124240：cmd 5，
payload 0x20=scene、0x24=stream。VBC_MUX_ADCn_IIS_PORT_SEL 存 startup 0x7c
（vbc_mux_iis_rx_put 0x11b08c），RX 宽度存 0xf4（vbc_put_iis_rx_width_sel 0x11c64c）。
原厂 DT `sprd-pcm-audio` 的 normal_c_l/normal_c_r 为 AGCP DMA 请求 3/4。

### IIS1 源选择（采集无 DMA 请求的根因）

ODM `codec_c` 路由打开时 `ag_iis1_ext_sel_v2=aud_4ad_iis0_ad0`，关闭时恢复 `pad_top`。
`vbc_put_ag_iis_ext_sel_v2` 0x11d4e4 → `arch_audio_iis_to_audio_top_enable_v2` 0x11f288：
AGCP AHB（`sprd,syscon-agcp-ahb`，0x56200000）0x3c，iis1 字段 mask 0xc、shift 2；
枚举文本指针（vbc-v4 .rodata 0xde90）为 {pad_top, aud_4ad_iis0_ad0, pdm_top_iis1}，
取值 1。未设置时数字 codec 的 ADC0 IIS 不进入 VBC IIS1：DSP 不写 AP01 录音 FIFO，
AGCP DMA 通道 3/4 已启用但源地址不动，PCM 在 buffer×1.1/rate 后 READN 返回 EIO。
本驱动在 capture startup（DSP cmd 4 之前）写 1，shutdown（cmd 5 之后）写 0。

实测（2026-11-09，6.18.35-w202ds-m23-audio-20261109）：`plughw:0,1` 48 kHz S16 立体声
5 s，安静环境左（MIC1）RMS 18（−65 dBFS）、峰值 110，右（MIC3）RMS 60（−55 dBFS）、
峰值 258，无数字静音；扬声器播放 440 Hz 时两路在 440 Hz 的幅度为 163 / 183，
其余频点 ≤7。PipeWire 默认 profile set 只探测 PCM 0，`w202ds.conf`（udev
`ACP_PROFILE_SET`）同时映射 `hw:%f,0` 输出与 `hw:%f,1` 输入。
