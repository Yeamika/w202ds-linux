# W202DS Boot

本分支保存 W202DS 启动工程的远程入口说明。当前远端只提交本 readme.md，实际代码仅在本地维护和提交，未经明确指令不推送。

- 远程：Yeamika/w202ds-linux，分支 boot。
- 本地工程：/state/workspace/pushup/w202ds-boot。
- Linux 内核：同一远程的 6.18.35 分支，本地 /state/workspace/pushup/w202ds-linux/6.18.35。
- Linux EFI 加载器：pocketjs4edk2_uefi/efi/LinuxBoot。
- EFI 公共交接层：现有加载器内部 common/，本地链接 pj4edk2-linux-efi，远程对应 pj4edk2 的 linux-efi 分支。
- 发行版组装：本地 /state/workspace/pushup/reimuks；设备入口为 w202ds/kernel 与 w202ds/boot。

本地保留 LK、UEFI 板级实现、原提交历史及构建入口；远端文档初始化不代表代码已经发布，也不代表完成实机启动验收。根分区与 initramfs、显示的交接规范见本地 reimuks/boot/doces/efi_dtbchosen.md。
