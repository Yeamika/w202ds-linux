# w202ds-camera-driver（摄像头）

```text
camera/
├── build.mk
├── devicetree/
├── source/w202-flash/       # 独立 AW36518A LED class 驱动
├── userspace/              # udev 权限模板与离线安装入口
└── README.md
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| 摄像头识别 | 原系统有型号记录，Linux 待验 | 原系统报告 GC13A0 与 SC520CS；前后摄对应关系未确认。 |
| 图像采集与预览 | 待完成并验证 | 尚无 Linux 实际采集与预览验收结果。 |
| 拍照 | 待完成并验证 | 传感器及 ISP 链路尚未验收。 |
| 录像 | 待完成并验证 | 尚无 Linux 实际录像验收结果。 |
| 后置手电筒 | 驱动与构建入口已提供 | AW36518A，标准 LED class，0 关闭、1～8 亮度；当前整合版待实机验证。拍照闪光与预闪未实现。 |

手电筒独立使用当前设备树的 `sprd,flash-aw36518a_gpio` 节点，不依赖摄像头采集
或 ISP 驱动。构建目标为 `w202ds-camera-flash`，须显式选择；安装和接口见
[userspace README](userspace/README.md)。摄像头采集链路的状态不因手电筒接入改变。
