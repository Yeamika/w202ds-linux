# W202DS 音频用户态

`rootfs/` 提供扬声器 / 麦克风初始化脚本和 `w202-audio.service`，作为唯一维护的音频启动实现。脚本检查音频模块的 vermagic 与运行内核一致，按序加载模块，等待时钟和 ALSA 节点就绪，不播放开机音。

```sh
make install DESTDIR=/path/to/staging
```

安装仅暂存文件。发行版负责提供匹配模块、固件、kmod 和 systemd，并决定是否启用服务。REIMUKS 检测到音频声卡模块时使用本模板。ALSA UCM、路由和音量配置也归本组件；PipeWire/WirePlumber 由发行版提供。

系统集成负责安装模板和桌面策略，应用使用此组件说明的 Linux 标准接口。

`rootfs/usr/share/alsa-card-profile/mixer/profile-sets/w202ds.conf` 与 udev 规则
将扬声器 PCM 0、麦克风 PCM 1 交给桌面的 ACP / PipeWire。modprobe 黑名单保证
音频驱动由初始化脚本按序加载；旧播放 DT 不要求麦克风模块或录音节点。
脚本按声卡 ID 查找设备，不假设该卡一定是 card 0。
