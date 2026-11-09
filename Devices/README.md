# W202DS 硬件

✅ 已验证可用 · 🟡 部分可用 · ❓ 待验证 · ❌ 尚未实现或当前不可用

| 硬件 | 芯片型号 | 日常可用性 | 描述 |
| --- | --- | :---: | --- |
| [处理器调频](cpu/README.md) | UMS9620（T760）：4×Cortex-A55 + 4×Cortex-A76 | ✅ | 三簇 schedutil 调频已验证；开机约 45 秒后启用，电压偏移不支持，温控限频待验。 |
| [屏幕](drm/README.md) | HX83102J；UMS9620 DPU / DSI / DSC | ✅ | 显示与登录可用；KDE 桌面、长时间运行及休眠恢复待验。 |
| [图形处理器](gpu/README.md) | ARM Mali-G57 | 🟡 | 基础 3D / GLES 及四档动态调频已验证；桌面、浏览器及 Vulkan 待验。 |
| [视频编解码器](vpu/README.md) | UMS9620 集成 VPU | 🟡 | H.264 解码已验证；普通播放器、浏览器硬解及编码待完成。 |
| [Wi-Fi / 蓝牙 / 卫星定位](wcn/README.md) | SC2355 / Marlin3 | 🟡 | Wi-Fi 联网与热点分别可用，模式二选一；蓝牙扫描、配对及经典蓝牙主从加密短测已验证；业务传输、音频、BLE、休眠与定位待验。 |
| [蜂窝基带](modem/README.md) | UMS9620 集成 Modem | 🟡 | SIM 识别、注册与蜂窝上网可用；通话、短信等待验；ModemManager 原生 5G 连接已测，持久服务接入待完成。 |
| [电源管理与电池监测](pmic/README.md) | UMP9620；设备树另含 UMP9621 / UMP9622 | 🟡 | 电池电压、电流、温度等可读；电量百分比为估算，马达和呼吸灯待验。 |
| [充电控制](charging/README.md) | 双路 UPM6910H（兼容 BQ2560x） | 🟡 | 停充保持与外接电源负载运行已测；完整充电切换、快充及电池隔离待验。 |
| [合盖霍尔开关](hall/README.md) | GPIO148 霍尔开关，具体芯片型号未确认 | ✅ | 合盖灭屏、开盖亮屏已验证，用户确认正常；长期运行待验。 |
| [键盘与触控板](keyboard/README.md) | ZSK UART 键盘方案，控制芯片型号未确认 | 🟡 | 键盘由用户确认正常；触控板有历史验证，完整功能与 KDE 待验。 |
| [运动、磁场与光线传感器](sensorhub/README.md) | LSM6DSOQ（加速度 / 陀螺仪）、MMC5603（磁力计）、LTR578（光线） | 🟡 | 10 通道原始数据可读；方向、单位、校准及自动旋转 / 亮度待验。 |
| [触摸屏](touchscreen/README.md) | Himax HX83102J | ❓ | 有历史 Linux 输入记录；当前触摸及固件重载待复测，默认未启用。 |
| [USB / Type-C 接口](usb3/README.md) | UMS9620 DWC3 / USB PHY；外接接口芯片型号未确认 | ❓ | USB 串口 / 网络有历史验证；OTG、PD、DP 外显及网卡共存待验。 |
| [扬声器与麦克风](audio/README.md) | UMP9620 音频 codec；AW883xx 功放（具体型号未确认） | ❌ | 播放、录音及完整音频链路尚未验收。 |
| [摄像头](camera/README.md) | GC13A0（1300 万像素）、SC520CS（500 万像素），原系统报告 | ❌ | Linux 拍照与录像尚未验证；前后摄对应关系待确认。 |
| [神经网络处理器](npu/README.md) | IMG AX3xxx 系列 NNA（设备树标识，具体型号未确认） | ❌ | 尚无可运行、经验证的推理功能。 |
| [视觉数字信号处理器](vdsp/README.md) | UMS9620 集成 VDSP | ❌ | 尚无可交付、经验证的独立运行功能。 |
| [内置存储](storage/README.md) | eMMC（厂商 / 料号未确认）；UMS9620 SDHCI | ❓ | 当前配置启用 eMMC；独立读写与长期稳定性验收待补。 |
| [温度监测与热管理](thermal/README.md) | UMS9620 温度传感器；UMP9622 tsensor / PMIC ADC | ❓ | 温度精度、校准、限频与过热告警尚未验收。 |
| [系统挂起唤醒](suspend/README.md) | UMS9620 LP_AP_INTC、AP EIC / GPIO | 🟡 | deep 挂起只由电源键与 RTC 唤醒，RTC 30 / 60 / 90 秒满时长挂起及触摸不唤醒已验证；长时间耗电与联网挂起待验。 |

更新日期：2026-11-06
