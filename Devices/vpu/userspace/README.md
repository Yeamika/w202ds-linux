# W202DS VPU 用户态

`source/` 保存解码接口诊断、AVC worker、码流生成和运行库打包工具。共享接口头位于 `../source/`，按相对路径直接引用。

Linux libc 客户端和原厂 AArch64/Bionic worker 分开构建，不能将 glibc 程序与 Bionic 库混合链接。聚合用户态入口不隐式构建专有依赖。

```sh
# 主机范围算法回归，不打开设备。
make OUT=/path/to/native-tests check
# ARM64 范围测试；诊断客户端另需包含 drm/panfrost_drm.h 的 UAPI 头目录。
make OUT=/path/to/arm64 all
make OUT=/path/to/arm64 CPPFLAGS="-I/path/to/uapi" diagnostics
```

`w202-avc-worker.c`、`test-vendor-avc.c` 和诊断挂钩使用原厂 Bionic ABI，需要单独提供匹配的链接环境及设备自身 AVC 库，不由此 Makefile 编译或分发。`package-vpu-runtime.py` 只整理显式传入的提取物，不提供专有库。

`make-avc-fixture.py` 和 `make-runtime-fixtures.py` 需要 PyAV、NumPy/libx264，生成软件参考用于对照，不作为硬件解码成功依据。

## 解码服务

```sh
make DESTDIR=/path/to/staging install
```

该入口只安装 `rootfs/` 中的服务及命令模板，不加载模块或启用服务。准备配套的
`w202_vpu_power`、`w202_vpu_iommu`、`w202_vpu_codec` 模块，并单独提供设备自身的
Bionic 运行库和 `/opt/w202ds/vpu/android/w202-avc-worker` 后，可使用：

```sh
sudo w202-vpu start
sudo w202-vpu decode INPUT.mp4 OUTPUT.y4m
sudo w202-vpu stop
```

启动脚本按实际 `uname -r` 检查三个已安装模块的 vermagic，不限制固定的内核提交或
版本名称；更换内核仍须提供对应构建的模块。已有模块还会核对 srcversion 和设备状态，
不强制加载不匹配模块，也不卸载正在使用的解码器。运行依赖包括 bash、kmod、
util-linux、coreutils、ffmpeg、匹配的 GPU 服务及原厂解码运行库；安装入口不下载这些依赖。

`libva-v4l2_request/` 为忽略的可选实验检出树，固定提交 `4f60c51f7841dd74bb61c94a4ad31e630bbdab01`；适配补丁在 `source/libva-v4l2-request-w202-profile.patch`。其独立 Meson 构建需要 libva 及 DRM UAPI 开发文件。

标准接口和能力限制见 [v4l2.md](../source/v4l2.md)。本目录不混入运行包、私有库或历史构建输出。

系统集成负责安装模板和桌面策略，应用使用此组件说明的 Linux 标准接口。
