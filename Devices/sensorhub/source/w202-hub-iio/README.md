# W202DS CH Sensor Hub IIO

本驱动经现有 SIPC dst9/channel4 的两个 sbuf RX 环接收 CH 固件数据，用两个阻塞接收线程等待事件，不注册/覆盖 spipe notifier，不复位 CH 或 modem。传感器共享同一传输和固件，保持一个模块拥有接收环。

## 前提：CH 共享内存必须非缓存映射

spipe_ch 的环头与收发缓冲在 CH SRAM `0x6500d000`，该区域没有 `struct page`。CH 不监听 AP 缓存，因此不能回退到 WB 映射。当前 SIPC 使用 `ioremap` 的 Device-nGnRE 映射，保留 PM 固件启动所需的内存属性；SBUF 内核缓冲区收发及回绕分支使用标准 `memcpy_toio/fromio`，避免通用 memcpy 尾部的未对齐访问。

2026-11-08 实机曾在旧拷贝路径发生两次对齐异常，修复后联合采样和空闲关闭正常。SBUF 用户缓冲区拷贝及其他共享内存访问路径的完整审计不属于此次验证范围。

## 来源

基于 GeekerCloud 提供的 sensorhub 工作树（Git 基线 `a6ae919feda1410490fa41a15628a32089987554`）适配。本机补充 Device SRAM 的 SBUF I/O 拷贝修复及按变化上报的光照缓存语义，验证范围如下。

## 固件与器件

探测时依次查询固件版本（0xcd）、传感器列表（9），发送 AP/CH 时钟映射（0xc4，此后每 10 秒续同步）与 host normal（0xc5）。CH 在收到时钟映射前不上报任何数据。接受以下两个已用实测数据核对上报格式的 CH 映像，其余版本拒绝（`-EPROTONOSUPPORT`）：

| CH 固件 | 加速度/陀螺 | 磁力计 | 光线 |
| --- | --- | --- | --- |
| 20250605 | acc/gyro_lsm6dsoq（ST） | mag_mmc5603（MEMSIC） | light_ltr578 |
| 20250219 | acc/gyro_lsm6dsoq（ST） | mag_mmc5603（MEMSIC） | light_stk3x1x（Sensortek） |

句柄固定为 1 加速度、2 磁力计、4 陀螺、5 光线；光线设备按 CH 报告的芯片命名。

## IIO 接口

| IIO 名称 | 通道 | 单位 |
| --- | --- | --- |
| w202-lsm6dsoq | in_accel_{x,y,z}_raw × in_accel_scale；in_anglvel_{x,y,z}_raw × in_anglvel_scale | raw 为 µm/s² 与 µrad/s，scale 0.000001 得 m/s² 与 rad/s |
| w202-mmc5603 | in_magn_{x,y,z}_raw × in_magn_scale | raw 为 nT，scale 0.00001 得 Gauss |
| w202-ltr578 或 w202-stk3x1x | in_illuminance_input | lux（Hub 光照协议直接给出） |

`raw` + `scale` 是 iio-sensor-proxy 轮询加速度驱动（`iio-poll-accel`）识别的接口。三个设备都带 `hub_stats`：固件版本、各句柄样本计数与 Hub 微秒时间戳、两个环的坏帧计数。

## 采样方式

首次读取发送 batch（请求 200 ms 周期）和 enable，等待启用后的首个样本。运动传感器样本超过两个请求周期加 100 ms 未更新时等待新样本，最长 2.5 秒。环境光按变化上报，启用后保留最近有效值，不要求亮度不变时也产生周期性新帧。5 秒无人读取即由空闲任务关闭。浮点值以定点整数解析，不使用内核 FPU；卸载时关闭本模块开启的传感器并停止接收线程。

## 构建与测试

本地独立构建：

```sh
make -C /path/to/Devices KDIR=/matched/kernel/output OUT=/new/work/modules w202ds-sensorhub-iio
```

自动先构建匹配的 mailbox、power manager、SIPC core 符号提供者，目标产物是 `OUT/w202ds-sensorhub-iio/w202_hub_iio.ko`；不会安装或重载正在运行的基础模块。协议解析的独立主机测试：

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror test_protocol.c -o /work/test-protocol
/work/test-protocol
```

`w202_hub_protocol.h` 被驱动与该测试共同使用，覆盖真实帧、双环碎片、CRC 损坏恢复、最大帧和数值异常拒绝。

开机加载与 iio-sensor-proxy 配置见 [用户态集成](../../userspace/README.md)。原始 `w202-hubctl` 和本模块会消费同一接收环，不能并行采样；时钟同步诊断模块不应与本模块的周期同步重复运行。

## 验证与 TODO

用户已验收本轮加速度、陀螺仪、三轴磁场和环境光的读取及动态响应。

本轮实机为 20250605／LTR578。静置 60 秒／277 轮，以及用户转动约 175 秒／832 轮联合读取均无错误，双环坏帧均为 0；协议解析测试通过。转动时加速度、陀螺仪、磁场均有变化，光照范围 0～325 lux，SensorProxy 捕获四种平面方向。20250219／STK3x1x 是兼容代码，未在本机验证。

匹配内核未启用 IIO_BUFFER，只提供直接读取。已验证本轮运动和光照动态响应；安装矩阵与实物方向的对应、校准、系统挂起、长期稳定性及 KDE 画面联动仍待验。
