# USB-C 一线通用户态

本目录是 USB-C PD / DP Alt Mode（一线通）在用户空间的配置与脚本，与内核 `ump9620-tcpc.ko`、`../source/w202-dp/` 配套。内容取自当前真机运行副本，逐文件哈希见下表。

| 文件 | 安装位置 | 作用 |
| --- | --- | --- |
| `rootfs/etc/modprobe.d/w202-usbc-pd.conf` | `/etc/modprobe.d/` | `ump9620_tcpc enable=1`（驱动默认关闭）；禁止加载只做插拔检测的旧 `sc27xx_typec`，二者共用 Type-C IRQ。 |
| `rootfs/etc/udev/rules.d/73-w202-usbc-drswap.rules` | `/etc/udev/rules.d/` | `port0-partner` 出现时用 `systemd-run` 异步执行下面的脚本。 |
| `rootfs/usr/local/sbin/w202-usbc-drswap` | `/usr/local/sbin/`（0755） | 等待 3 秒；仅当本机为 sink、UFP（device）且对端支持 PD 时，向 `/sys/class/typec/port0/data_role` 写 `host`。TCPM 不会主动发起 DR_Swap，而给本机供电的显示器 / 扩展坞需要本机做 DFP 才能进入 DP Alt Mode。只尝试一次，不重试。 |
| `integration/udev/rules.d/72-w202-mutter.rules` | `/usr/lib/udev/rules.d/` | 可选 GNOME 集成：内屏 `w202-r6p0-lk-dsi` 为 Mutter 主 KMS 设备，Panfrost 主节点忽略，`w202-dp` 不被忽略且不成为主 GPU。KDE 未验证。 |

```sh
make DESTDIR=/离线rootfs暂存目录 install
```

`install` 只复制文件到显式 `DESTDIR`，不启用服务、不加载模块、不触发 udev。

## 与现有模块的关系

- 不要同时启用旧的 OTG 自动切换（强制 I2C 打开充电芯片升压位的 `w202-usb` 脚本及其 udev / systemd 单元），也不要保留 `modules-load.d` 中的 `sc27xx_typec`：角色和 VBUS 由 TCPM 管理。
- `ump9620_tcpc` 与本仓 `Devices/pmic` 的 `w202_ump9620_typec.ko` 都匹配 `sprd,ump9620-pd`，`ump9620_tcpc` 还要求树内 `bq256xx_charger` 以 `bq256xx-charger` 名称绑定主充电芯片，与 `Devices/charging` 的 `w202_bq_control.ko` 匹配同一 `ti,bq2560x_chg` 节点。两套只能择一，见 `../README.md`。
- HP Lapdock 不支持，本目录没有针对 Lapdock 的脚本或规避措施。

## 来源与哈希

| 文件 | SHA256 | 与真机副本 |
| --- | --- | --- |
| `w202-usbc-pd.conf` | `dcc158dc1c61fd47431db0a03c718c5dea0e602db9ed923e955ab879782977be` | 删去一行指向本机旧测试文件的回退说明注释，其余逐字相同（真机 `8e429faf…0a990a09`）。 |
| `73-w202-usbc-drswap.rules` | `6dde4a76c2a4a3d53b434c880a6dd5f081a3e3d15a6d208c03a14565f9e2c807` | 相同 |
| `w202-usbc-drswap` | `d0fdd3d6a24ed3dcc000993d4e2547fec7f0f53156d2df1b34c31a111e2f4e17` | 相同 |
| `72-w202-mutter.rules` | `70b9803c3036114338996b133ea537176281b2f401f25192263cb119e2f0f463` | 相同 |
