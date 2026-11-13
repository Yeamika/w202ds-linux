# W202DS 键盘用户态

- `source/keyboard-attach.c`：配置 UART 并保持 line discipline。
- `rootfs/`：前台 attach 的 systemd 基础服务。
- `tests/`：主机协议测试与 UART 诊断。

内核驱动和共用协议头在 `../source/keyboard-m22/`，测试直接引用协议头。驱动提供标准按键、触摸板和 `SW_TABLET_MODE` 状态。

```sh
make OUT=/path/to/output all diagnostics
make OUT=/path/to/output DESTDIR=/path/to/staging install
make OUT=/path/to/native-tests check
```

安装只暂存硬件适配，不启用服务或修改桌面会话。是否启动 Maliit、实体键盘拆装后是否启用虚拟键盘等策略，由 REIMUKS 的 `devices/w202ds/rootfs/profiles/desktop/kde/` 维护。

系统集成负责安装模板和桌面策略，应用使用此组件说明的 Linux 标准接口。
