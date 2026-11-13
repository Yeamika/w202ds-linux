# 后置手电筒

AW36518A 驱动提供标准 LED class：`/sys/class/leds/white:torch/`。
`brightness` 为 0 时关闭，1～8 是递增亮度；`max_brightness` 为 8。
最高档使用厂商手电筒电流代码 `0x4a`，不提供拍照闪光、预闪或温控调节。

## 构建与安装

从板级源码根目录构建与内核匹配的模块：

```sh
make -C Devices KDIR=/matched/kernel/output OUT=/path/to/modules w202ds-camera-flash
make -C Devices/camera/userspace install DESTDIR=/path/to/rootfs
```

模块输出为 `OUT/w202ds-camera-flash/w202_flash_aw36518.ko`。模块安装和 `depmod`
由系统集成完成；现有设备树的 I2C modalias 可触发模块加载，不需要轮询 GPIO。
安装命令只暂存 udev 规则，不加载模块、不修改运行中系统。

规则允许 `video` 组写 `brightness`；发行版负责提供该组并决定用户授权。
应用可使用桌面 LED 服务或直接读写标准属性：

```sh
cat /sys/class/leds/white:torch/max_brightness
printf '1\n' > /sys/class/leds/white:torch/brightness
printf '0\n' > /sys/class/leds/white:torch/brightness
```

驱动在卸载、关机及挂起时关闭 LED，恢复时按 LED core 的亮度状态处理。
I2C 亮度更新失败时尝试关灯。具体亮度、挂起恢复、连续点亮温度与桌面联动仍需
在匹配硬件上验证。桌面开关、服务启用及用户策略由 REIMUKS 定义。
