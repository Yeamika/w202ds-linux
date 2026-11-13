# 传感器桌面集成

使用 Debian `iio-sensor-proxy`，通过标准 SensorProxy D-Bus 接口向桌面提供加速度方向和环境光；不另写轮询守护程序。陀螺仪、三轴磁场由 IIO 提供，不伪装成已校准罗盘。

| 文件 | 作用 |
| --- | --- |
| `rootfs/usr/lib/systemd/system/w202-sensorhub.service` | 基带运行时启动 CH 后加载模块，随基带运行时停止。 |
| `rootfs/usr/libexec/w202-sensorhub-attach` | 加载模块并等待 CH 收发环就绪，最多尝试绑定 30 次；绑定成功立即返回。整个服务启动超时为 60 秒，包含模块加载及 probe 耗时。 |
| `rootfs/usr/lib/udev/rules.d/81-w202-sensorhub.rules` | 配置 LSM6DSOQ 安装方向矩阵。 |

## 安装

```sh
make install DESTDIR=/path/to/rootfs
```

此命令仅暂存文件。将匹配的 `w202_hub_iio.ko` 安装到 `/lib/modules/$(uname -r)/updates/w202ds/`，运行 `depmod -a`，安装发行版 `iio-sensor-proxy`，启用 `w202-sensorhub.service`。CH 由现有基带运行时启动；此处不下载固件、不写 NV、不复位处理器。

安装时须同时提供现有基带运行时的服务、工具、匹配模块及固件；单独安装 Sensor Hub
服务不能启动 CH。组件选择、服务启用和桌面策略由 REIMUKS 负责，本目录只提供模板。

## 离线检查

```sh
make check OUT=/path/to/native-output HOSTCC=cc
```

此入口运行现有 CH 协议测试，检查帧向量、分片、双接收环、校验恢复和浮点转定点，
不访问硬件。从仓库根目录执行 `make -C Devices/sensorhub/userspace check` 可运行该检查。

## 上层接口

按 `/sys/bus/iio/devices/iio:device*/name` 查找设备，不固定编号：
`w202-lsm6dsoq` 提供加速度及角速度的 `raw` / `scale`；`w202-mmc5603` 提供三轴
磁场的 `raw` / `scale`；`w202-ltr578` 或 `w202-stk3x1x` 提供处理后的照度。
通道及单位见 [驱动说明](../source/w202-hub-iio/README.md)。

运动数据按新鲜度等待新样本；环境光是 on-change 上报，启用后的最后一个样本在
光照不变时仍有效。停止读取 5 秒后关闭采样，下次启用重新等待首帧。该缓存行为须保留。
键盘拆装通过独立 input 设备 `W202DS Tablet Mode` 上报 `SW_TABLET_MODE`，不依赖 CH。

2026-11-08 本机 KDE 上，SensorProxy 已报告 `HasAccelerometer=true`、`HasAmbientLight=true`，可输出 right-up 方向和约 160 lux。SSH 会话不是本地活动桌面会话，命令行验收可用 `sudo monitor-sensor --accel --light`；不为远程测试放宽传感器访问策略。

## 方向与验证边界

原生面板为竖屏 1200×1920，键盘支架横屏时原始重力主分量在 +X；当前矩阵 `diag(-1, -1, 1)` 在此姿态报告 right-up。转动记录已捕获 normal、left-up、right-up、bottom-up 四种方向和倾斜变化；每个状态与实物方向的对应仍待标记核对。自动旋转及亮度策略由桌面管理，其界面和联动效果未验收。

本轮传感器数据读取与动态响应已获用户验收。

TODO：核对带方向标记的安装矩阵、桌面联动和挂起恢复；光强标定另验。

系统集成负责安装模板和桌面策略，应用使用此组件说明的 Linux 标准接口。
