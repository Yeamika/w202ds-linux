# RAMLK 构建

```sh
python3 lk/legacy-buildchain/ramlk/build.py --output /path/to/new-output
```

依赖 Python 3 和 aarch64-linux-gnu-gcc/ld/objcopy，可用 `--cross-compile` 指定工具前缀。输出目录必须位于仓库外且尚不存在。

默认读取 `lk/firmware/uboot-mod.bin`，校验后补零到 5 MiB，生成相同的 `uboot_a.img`、`uboot_b.img`；随后修改 B 的日志标记、编译并注入 `hook.S`，生成 `lk-jump-rmk.img`。所有阶段使用锁定哈希检查，记录于输出的 `manifest.json`。

- 补零镜像：`9bfdec9dd4b053eb676e0fc003099bb42b76090d22cdac9f860bfb578b887d99`
- hook：`7a0b106574f4c0c6fcb78a0eecc4fa0dc8756de8753ae4020f0fa20963ddd5a7`
- RAMLK：`fe2c7b3f9384453aa9fe68c3646efd1705a221fe17a0e0d4005dd2fa89051a42`

此过程重编译新增 hook，不重编译原生 LK 主体。RAMLK 不包含 EDK II/PocketJS；后续由 `../ramboot/` 封装 UEFI。组合构建会自动临时生成 RAMLK，无需保存可再生镜像。
