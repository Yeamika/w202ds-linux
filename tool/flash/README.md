# W202DS FDL 刷写与分区读取

Python 3，支持 Windows/Linux。`loaders/` 包含 FDL1、FDL2 和必需的辅助载荷，运行前校验哈希。
`flash.py` 写入 U-Boot，`read.py` 读回分区；两者共用同一套 loader 校验、`spd_dump` 查找和预演约定。

在仓库根目录运行：

```sh
# 预演：检查文件并显示命令
python3 tool/flash/flash.py --image lk/firmware/uboot-mod.bin

# 加载到 FDL2，进入交互模式
python3 tool/flash/flash.py --execute

# 写入镜像并重启
python3 tool/flash/flash.py --image output/uboot_a.img --execute
```

Windows 使用 `python`。默认从 `spd_dump/win/`、`spd_dump/linux/` 或 PATH 查找程序；执行时找不到会自动下载当前平台的上游预编译程序，也可通过 `--tool` 指定。预演不下载。使用 Channel9 版本的 Windows 程序时，配套 DLL 放在 EXE 同目录。

流程：BROM → 发送 FDL1 → 发送辅助载荷 → 进入 FDL1 → 下载并执行 FDL2 → 写入镜像 → 重启。未指定 `--execute` 时不访问设备。

**FDL2 写入 B 可能连带覆盖 A，`--partition` 不保证单槽隔离。** 本入口仅写 U-Boot，不修改 boot、misc 或 verity。

## 读取分区

```sh
# 预演：查看将执行的命令
python3 tool/flash/read.py --partition uboot_b --out backup/uboot_b.img

# 打印设备分区表（含大小），读完自动 reset
python3 tool/flash/read.py --list --execute

# 整分区读回
python3 tool/flash/read.py --partition uboot_b --out backup/uboot_b.img --execute

# 只读分区中的一段
python3 tool/flash/read.py --partition uboot_b --offset 0x1000 --size 0x100000 \
  --out backup/uboot_b-head.bin --execute
```

`--partition` 接受分区名或分区号，`--offset`/`--size` 接受 `0x` 前缀；省略 `--size` 时由设备给出整个分区大小。
`--out` 省略时写到当前目录的 `<partition>.bin`，目标已存在则拒绝覆盖，需显式 `--force`。
写完打印文件大小与 SHA-256；默认读完 `reset`，`--no-reset` 保留在 FDL2 会话中继续操作。

读取同样经过 BROM → FDL1 → FDL2，必须先把设备切到下载模式；`--wait` 是等待下载口的秒数。
只读命令不写入任何分区，但 FDL2 本身以最高权限运行，操作前仍应确认分区名。

源码位于 Git 子模块 `spd_dump/source/`，使用 `git submodule update --init tool/flash/spd_dump/source` 获取。预编译程序来自 `TomKing062/action_spd_dump_it` 的最新 main 构建，下载记录包含构建标识和哈希；win/linux 缓存不提交。下载遵循标准代理环境变量。
