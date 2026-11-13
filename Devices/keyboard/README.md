# w202ds-keyboard-driver（键盘与触控板）

```text
keyboard/
├── build.mk
├── devicetree/
├── README.md
├── source/keyboard-m22/      # UART 键盘／触控板与连接检测
└── userspace/               # UART attach、服务模板和协议检查
```

用户态程序、服务模板和诊断见 [userspace](userspace/README.md)。

键盘套拆下时 `W202DS Tablet Mode` 上报 `EV_SW/SW_TABLET_MODE=1`，接回上报 0；
桌面通过标准 input 接口消费状态。KDE 虚拟键盘与旋转等行为由 REIMUKS 定义。

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| 键盘输入 | 用户确认正常 | 最新完整内核的已于 2026-11-05 确认。 |
| 触控板输入 | 有历史验证，当前待复测 | 历史 Weston 场景有记录；当前完整内核及 KDE 待验。 |
| Fn 组合键与修饰键 | 待完整验证 | 尚无全部组合键验收结果。 |
| Caps Lock 指示灯 | 待验证 | 尚无完整指示灯联动验收结果。 |
| 拔插与重新连接 | 用户确认可用 | 三轮拆下／接回，六次标准平板模式事件均到达 KDE；装回后键盘报告正常。 |
| 虚拟键盘联动 | 用户确认可用 | 拆下键盘套后启用 Maliit，接回关闭；使用标准 KDE 事件，无轮询。 |

更新日期：2026-11-08
