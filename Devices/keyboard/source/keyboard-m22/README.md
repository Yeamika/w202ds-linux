# M22 R4：原厂 UART 键盘和触控板

当前目录只维护内核实现与共用协议头。用户态 attach、基础服务及诊断已归入 [用户态目录](../../userspace/README.md)，分别构建；KDE 行为由 REIMUKS 定义；下文保留原验证记录。

## 键盘套拆装与平板模式（2026-11-08）

GPIO 检测键盘套连接状态，新增标准 `EV_SW/SW_TABLET_MODE` 输入设备
`W202DS Tablet Mode`：拆下为 1，接上为 0。连接 GPIO 的 IRQ 经 100 ms 去抖工作处理，
不增加周期轮询。键盘与触控板共用这个连接状态；拆下时释放已按下的键、触点和左键，
清空未完成的 UART 帧，断开期间忽略输入，接回后继续使用已有 TTY 通道。

2026-11-08：本地按 `6.18.35-w202ds-g102e847e2492` SDK 编译、热加载通过，
标准 switch 已注册；根文件系统与 KDE 启动镜像内的模块同步。KDE KWin 已报告
`tabletModeAvailable=true`。实机完成三轮拆下／接回，内核六次模式事件与 KWin
六次 `tabletModeChanged` 一一对应；拆下为 true、装回为 false。用户确认此项可用；
装回后捕获到六个键盘报告，UART／帧校验错误均为 0。该次捕获没有触控板报告，
不据此额外宣称触控板数据通路的新验收结果。挂起期间拆装和 IMU 自动旋转仍待验证。

## 历史实现与验证

以下为 M22 历史实现与验收记录；其中 PID、运行目录和 RAM 状态不是当前设备状态。当前构建入口见本硬件的 `../../build.mk`，不使用历史桌面验证脚本。

2026-10-26 UTC。用户已在Weston里确认键盘、鼠标均正常；驱动目前部署在M20启动的RAM rootfs，尚未合入boot分区。

输入设备为`W202DS ZTE Keyboard`、`W202DS ZTE Touchpad`。`/dev/ttyS2`使用115200/8N1、无流控；GPIO60 raw0表示连接，GPIO62 raw1供电。平台模块`w202_zsk`负责插拔/供电，内核line discipline处理输入，`keyboard-attach`只配置串口并保持打开。当前使用开发用途的`N_DEVELOPMENT=29`，不是已合入主线的serdev ABI。正常SIGTERM释放ldisc并恢复termios；不允许getty/ModemManager抢占ttyS2。

键盘帧为9字节：`01 modifiers XOR key1 key2 key3 key4 key5 key6`，全帧XOR为0。触控板为21字节，首字节07、四组手指坐标、时间、手指数、左键状态，**没有键盘式XOR字节**。R3误按22字节解析造成触控板无响应，R4已用真实原始报文修正。支持四个MT slot、按钮、修饰/按键释放与异常恢复；Caps LED反馈未实现，所有Fn/修饰键未逐项实体验收。

最终产物：

| 文件 | SHA256 |
| --- | --- |
| w202_zsk-input-r4.ko | 7eca0797920b990894fbb3bd348b60fc4019cef84c7411e847ae0405bb5c99e8 |
| keyboard-attach | d8a5f79fe00ef800842b9f70dc27027e839f90950f46187eccdef6ddb5e9fa33 |
| w202-keyboard-start | 1556ed4cbe49e3d976ba6e20082d3cb6b1c5b815122ac600ddb0fb67e6dea591 |

所有构建在CM2 `/root/debian-devices/work/w202ds/keyboard-m22/` 完成，模块W=1无告警、工具`-Wall -Wextra -Werror`通过，没有使用ASan。真实报文回放确认28个键盘帧及387个触控板帧，覆盖分包、粘包、键盘XOR、损坏流恢复及21字节触控板结构检查。

实机验收统计为42093字节、64键盘报文、1976触控板报文，0键盘校验错误、0TTY错误，1个触控板结构报文被拒绝、丢弃21字节。evdev记录真实键盘事件、X/Y移动、4次左键按下/松开与双指状态；用户确认正常，不宣称零丢包或所有手势均已验收。已验证正常退出、卸载/重载及启动脚本重复调用幂等。

证据：`../m21-debug-evidence/live-20261026/run/m22-keyboard/`，包含`accepted-input-state.txt`、`accepted-input-devices.txt`、`unloaded-gpio.txt`、两类原始串口报文、evdev原始事件与Weston日志。最后重载后统计归零，验收值应读取accepted文件。

历史验收时 RAM attach PID9120 与 Weston 保留运行，此状态不适用于本次源码导入。持久 rootfs 集成中的 systemd 服务采用标准版本模块目录和前台 attach；旧 `w202-keyboard-start` 针对历史 initramfs 平铺模块布局，已退出当前源码交付。
