# W202DS ALSA 扬声器与麦克风适配

四颗功放已由本机 I²C ID 确认为 **AW88166**（0x2066），地址为 bus8 的
0x34–0x37，共用 AON GPIO61。使用 Linux 上游 `aw88166` codec，原厂 ACF
保持原字节，仅安装名改为上游要求的 `aw88166_acf.bin`。

扬声器链路是 `AP01 PCM → AUDCP/VBC → master IIS0（SYS_IIS3）→ 四颗 AW88166`。
AP 普通 I²S 是蓝牙路径，AGCP_IIS0 是 DP 后端，两者均不是此声卡的 CPU DAI。

```text
audio/
├── build.mk                  六个独立外置模块及 mailbox 依赖
├── source/
│   ├── w202_audio_amp.c      共享 reset 与 codec 依赖
│   ├── w202_audio_dsp.c      AUDCP 启动、邮箱、共享内存 IPC
│   ├── w202_audio_vbc.c      AP01 DAI、扬声器路由与 DSP profile
│   ├── w202_audio_pcm.c      左右独立 SPRD DMA、planar PCM
│   ├── w202_audio_mic.c      UMP9620 模拟 codec 与 AGCP 数字 codec
│   ├── w202_audio_card.c     扬声器与可选麦克风两条 ASoC link
│   └── w202_audio.h          已核对的消息及 startup ABI
├── devicetree/w202ds-alsa.dtsi
└── firmware/prepare.py       校验原厂输入、裁剪 AVB 容器和生成参数包
```

用户态程序、服务模板和诊断见 [userspace](userspace/README.md)。

硬件格式限定为 **48 kHz、S16_LE、双声道、NONINTERLEAVED**。其他应用格式
由 ALSA plug / PipeWire 转换。当前实现不含耳机、通话、副麦 MIC2、运行时省电和
完整 suspend/resume；DSP、AP access 和相关电源域持续开启。

## 构建和设备树

内核的 `desktop` / `debug` 配置已包含 ALSA 和 DMA；其他配置可显式选用
`config/fragments/audio.config`。先完成匹配内核的 `Image modules` 构建。
随后按仓库构建策略执行：

```sh
make -C Devices KDIR=/path/to/prepared/kernel OUT=/path/to/modules w202ds-audio-driver
```

这会构建 `w202_audio_{amp,dsp,vbc,pcm,mic,card}.ko`，并复用 modem mailbox 模块。
音频目标已加入默认 SERVER_MODULES，desktop 配置包含匹配的 ALSA/DMA 支持。正式 `devicetree/w202ds-server.dts` 已包含
`w202ds-alsa.dtsi`，`w202ds-alsa-test.dts` 作为同一组合的兼容入口。它修改预留内存的映射属性和 GPIO 所有权，
**仅允许启动时使用，不能作为运行中 overlay**。

此配置禁用占用同一 GPIO61 的旧 DP 节点，将音频 reservation 标记为 no-map，
并通过 genpd 保证 DSP、时钟、DMA 和 codec 的依赖顺序。

启动顺序由 `userspace/rootfs/usr/libexec/w202-audio-start` 与
同组件的 `w202-audio.service` 管理；REIMUKS 读取该模板并启用服务，显式加载保持黑名单的模块，
应在内置 audio clock 的 deferred-probe 超时前启动。AGCP 时钟超过 deferred-probe 超时后仅补一次 bind，不做 unbind。

## 协议来源与验证边界

正式内核基线为 `102e847e249218775eef465c7b7c0a5cf78f040a`，使用
`CONFIG_LOCALVERSION_AUTO=y` 保留真实 Git 后缀。本机原厂模块通过 REA
反编译核对启动寄存器、IPC、VBC 和 GPIO 行为；实际 32 位
`audio.primary.whale.so` 核对参数选择编码，来源和 ABI 见
[source/PROTOCOL.md](source/PROTOCOL.md)。不引入整套原厂内核音频驱动。

2026-11-08：正式内核 `6.18.35-w202ds-g102e847e2492` 已完成 Image、393 个
内核树内模块和全部 44 个配套外置模块的完整构建，并安装、重启验证。
音频对象 `W=1` 通过；固件准备脚本的六项边界测试通过。
修复 PCM open 约束返回值和本机 AP DMA 中断号后，50 次连续 open/close、
144000 帧 / 2.997 秒硬件播放、标准 ALSA plug 与 12 秒 PipeWire 播放通过。
四颗功放播放时均为 SYSST=0x0311、SYSCTRL=0x5240（上电且解除静音），
停止后恢复关闭。这些底层和寄存器验证先在临时 `audio1` 内核完成；正式
内核又通过 50 次 open/close 与 144000 帧 / 2.997 秒播放。

正式内核上的 KDE/PipeWire 已播放左右声道提示音，用户确认实际听到声音。
Chrome 157 的 B 站 AAC 双声道音频已接入同一 PipeWire 扬声器输出，
后台已捕获 Chrome 播放流，视频未静音，播放期间 ALSA PCM 为 RUNNING。
AAC 由浏览器软件解码，采样率和布局由 PipeWire 转为本声卡支持的格式；
视频 H.264 的 VPU 硬解是独立路径。当前结果不覆盖麦克风、耳机或通话。

`w202ds-alsa.dtsi` 将 AP DMA 修正为 GIC INTID 88 / SPI 56，依据为本机播放时
的 GIC pending 与 DMA 状态；厂商 W205DS-ESIM 参考设备树的 SPI 55 在本机不适用。

## 录音与启动兼容

录音接入 MIC1（主麦，左）与 MIC3（背麦，右）：UMP9620 ADC → AGCP 数字 codec
→ VBC IIS1 → AUDCP AP01 录音 FIFO → DMA 请求 3/4。同一卡的 PCM 0 为播放，
PCM 1 为录音，格式同为 48 kHz、S16_LE、双声道 NONINTERLEAVED。
DSP 命令区分播放 / 录音 stream；AUDCP 释放复位后先发 doorbell 100，再等待
PMU 上电和启动通知。协议与寄存器来源见 source/PROTOCOL.md。

新驱动遇到没有 `zte,audio-mic` 的旧播放 DT 时仍注册扬声器；有 mic 属性时注册
第二条录音 link，并等待其 codec。更新必须交付匹配的完整模块组、DTB 和固件。
用户态按 ALSA card ID 定位声卡，ACP profile 同时暴露播放 PCM 0 与录音 PCM 1。

内核 `CONFIG_W202DS_AUDIO_LK_DT_FIXUP` 仅用于沿用原始 LK DT 的兼容启动，默认关闭。
它与已含 ALSA 的仓库 DT 不可同时使用；正常 EFI 启动读取完整仓库 DTB。
当前音频片段仍禁用争用 GPIO61 的旧 DP 节点，DP 共存需配套驱动确认 GPIO 所有权。

以上录音与兼容修改已接入源码；本仓库此前的扬声器实机记录不构成新增路径的实机认证。
