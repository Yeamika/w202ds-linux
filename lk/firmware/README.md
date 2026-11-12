# LK 固件输入

本目录使用 `uboot-mod.bin` 作为固定输入，不保存可再生的 A/B 镜像。

- 大小：2258808 字节。
- SHA-256：`a5686ba136bb845536aa2ef811b9c643a1bdac231ee838f36d159979c530c8b3`。
- 来源：`中兴云电脑WD20XDS半自动解锁Root与显串工具_v6.zip` 中的 `mod/uboot-mod.bin`。
- 来源压缩包 SHA-256：`69d00e40650682a557ff4696779c5b5b4c35874439b5507dcf163bdeafddaf9d`。

构建先补零至 5 MiB，再注入 RAMBoot、调试恢复和 UEFI 常驻桥接；设置保存在 ESP。
仅补零后的 A/B 基础镜像完全相同，SHA-256 为
`9bfdec9dd4b053eb676e0fc003099bb42b76090d22cdac9f860bfb578b887d99`。

构建命令与阶段校验见 [遗留构建链](../legacy-buildchain/README.md)。
输入固件受 Git 忽略规则保护，克隆代码后需自行提供上述匹配的文件。
原厂固件到工具包 mod 的源码生成链尚未找到；本工程可重建的是从固定 mod 输入开始的后续链路。
