# pj4edk-boot-display

DRM 前的可选启动日志实现，源码属于 Linux；没有导入 EFI、LK 或 PocketJS 运行时。
`CONFIG_PJ4EDK_BOOT_DISPLAY=y` 时内建，真实启用还要求 `/chosen/pocketjs,boot-display`
通过长度、格式、几何和已保留显存校验。缺少或无效描述时不注册此 console。

`printk` 回调更新文本，workqueue 异步画脏字符、清理缓存并提交继承的 DPU 帧。
硬件忙时延后重试，没有固定每行 50 ms/500 ms 延迟。持有显示所需的 6 个时钟引用，
防止共享 VPU 电源管理关掉还在扫描的时钟。

DRM 使用标准 aperture API 移除旧显示 owner；回调停止并等待绘制工作后释放时钟。
DRM 必须先取得自己的时钟引用。本组件不控制 DRM 的私有入口。

兼容当前已刷入的 UEFI：Linux 继续消费原有 80 字节描述，不要求新增字段或修改固件。
本轮只修改 Linux 侧实现，不需要重编或刷写 UEFI、LK、uboot。

## 规范与自定义接口

当前启动器是 EFI 应用，退出 Boot Services 后按 ARM64 Linux 原生协议进入 Image。
内核配置关闭 `CONFIG_EFI`，不接收 EFI Runtime Services。ARM64 的 DTB 地址、
CPU/MMU/缓存状态、内存与 initramfs 布局属于启动协议；黑框继承是本项目可选扩展。

显示描述在 DTB `/chosen/pocketjs,boot-display`，80 字节、20 个大端 u32。
EFI 内部发布的 BootDisplay 配置表使用另一份 36 字节矩形描述，启动器读取它，
核对 GOP/显示寄存器/保留内存后转换成这 80 字节；Linux 不使用 EFI 配置表中的原指针。

| 索引 | 当前约定 |
| --- | --- |
| 0～1 | 固定协议标记 `1, 2`，当前消费者严格校验 |
| 2～3 | 64 位显存物理地址，当前 `0xb0000000` |
| 4～5 | 保留区大小 `0x9e4000`；有效画面大小 4,608,000 字节 |
| 6～9 | 物理宽 1200、高 1920、stride 2400 字节、格式编号 1（RGB565） |
| 10～11 | 逻辑画布；当前 EFI 为 1920×1200，兼容旧 LK 的 1280×800 |
| 12～15 | 日志框 x、y、宽、高 |
| 16～19 | 旋转 90、面板安装方向 180、每像素 2 字节、固定标记 1 |

画面像素保留在 RAM，DPU/DSI/面板维持交接前状态；这些不是由 80 字节保存的寄存器快照。
`w202,uefi-profile` 和 `w202,uefi-load-us` 是另外两项自定义启动信息，不由本组件消费。

原生 DRM 的可选框内 console 也读同一份 DT 描述，但没有本组件的私有符号依赖。
描述缺失或格式错误时走普通全屏 fbdev；有效时继承画面，使用自己的 DRM buffer 继续输出。
标准协议分别见 [ARM64 Linux booting](https://docs.kernel.org/arch/arm64/booting.html)、
[UEFI System Table](https://uefi.org/specs/UEFI/2.10/04_EFI_System_Table.html)、
[Linux EFI](https://docs.kernel.org/arch/arm/uefi.html)。

## 验证与 TODO

完整内核构建检查内建接入；DRM 的 `source/baseline/tests/boot-display-contract.c`
覆盖缺失、损坏、越界和两种有效布局。测试说明见该目录 README。
分离后重新上机验证时钟交接、显示接管和异常退出；编译通过不等于真机通过。

## 公共层归属

本目录继续作为唯一 W202DS 启动显示模块。内部的 [common](common/README.md)
固定 pj4edk2 `kernel-boot-display` 分支；远程通过 gitlink 固定提交，本地为相对符号链接。
该公共依赖不在顶层 components 下另建并行驱动。

抽取边界见 [common/ABSTRACTION.md](common/ABSTRACTION.md)：公共描述与校验先行，
显存/保留区确认、DPU、时钟、电源、旋转与缓存操作留在本模块。
当前 common 仅有初始化 MD，Makefile、Kconfig 和 C 实现仍使用本目录原有代码；
后续公共 C 接口通过实际构建验证后才接入，不能将文档占位视为已完成通用化。
