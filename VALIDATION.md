# 2026-11-08 源码拆分验证

以固定 LK 二进制底包、同版设置区、重新构建的 EDK II 平台与 PocketJS 组件完成离线构建。来源及输入要求见 `inputs.lock.json` 和两个组件仓库的 `sources.lock.json`。

通过的检查：

- 平台源码、构建选项、公共 ABI、输入固件与平台产物身份校验。
- PocketJS 源码、完整归档和配套 ABI 清单校验。
- SDK 导出、ARM64 无未定义符号链接、PE 入口与 FV 校验。
- 新应用在内层 FV 中唯一存在；压缩 FV 往返一致；SEC 固定位置和重定位内容保持。
- RAMBoot 的 FV、TE、SEC 入口与容量检查。
- 重新编译 LK resident、返回桥接和 bootstrap；底包哈希、原生分支入口、不可变区域、签名副本和设置区保持检查。
- ARM64 Unicorn：4 项 resident 测试和 6 项返回测试通过，覆盖普通/外部复位、原生 recovery/fastboot/charging 路径、损坏/读取/显示/按键失败及返回上下文。

原 resident 测试只模拟了早期入口所需的系统寄存器。本次增加 EL2 寄存器与 GIC 模型，使测试执行生产构建 `LK_RETURN=1` 的实际保存代码，并核对传给 UEFI 的返回描述符。未通过修改桥接实现或跳过正常启动路径来消除测试失败；模型仍不能验证真实 GIC、MMU、缓存或电源时序。

本轮产物：

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `w202ds.fd` | 2097152 | `4deb6ed1811301a1ff61350ad1912ad8203ae757c2c8ad3a0eae29f6260bf6b6` |
| `ramboot.img` | 907264 | `155a09ce7b112317241d0346cd5091a876a644094e75634f2ee1c38089d4a522` |
| `resident/uboot-b-uefi.img` | 5242880 | `d63a58f08c9835b8e1bbe0336319f65ffae194d19509ad6cff7c4f39dadc16ad` |

三个 ESP EFI 加载器也已构建并按菜单路径导出。完整输入、产物哈希及封装报告由构建输出的 `manifest.json`、`resident/manifest.json` 保留。

完整 LK 底包仍是外部二进制输入；从完整 LK 源码重建该底包不在本次已验证范围内。本次没有刷写、重启设备或进行新固件真机验证。
