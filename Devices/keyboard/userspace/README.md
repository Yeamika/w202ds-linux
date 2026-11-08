# 电源键用户态

本目录管理 W202DS 电源键（设备树 `../devicetree/w202ds-keyboard.dtsi` 中的 `gpio-keys` 节点）在 GNOME 桌面上的用户空间处理：短按亮灭屏（手机式锁屏），按住 1 秒弹出电源菜单（关机 / 重启 / 挂起 / 取消），以及调整息屏、锁屏和挂起时间的设置程序。电源键与键盘同属输入设备，因此放在 `keyboard/` 硬件项目下。

| 目录 | 内容 |
| --- | --- |
| `rootfs/` | 按键守护进程 `w202-power-key`、其 systemd 单元、logind（电源键、合盖）与 dconf 配置模板，设置程序 `w202-screen-settings` 及其桌面文件。 |
| `power-menu/` | GNOME Shell 扩展 `w202-power-menu@w202ds` 源码及其简体中文翻译。 |
| `screen-lock/` | GNOME Shell 扩展 `w202-screen-lock@w202ds`（手机式息屏 / 亮屏 / 锁屏）。 |
| `tests/` | 通过 GNOME Shell 截图服务截屏的验收辅助脚本，以及 uinput 按键、触摸屏注入脚本。 |

## 已实现

`w202-power-key` 只读（不独占）`/dev/input/by-path/platform-gpio-keys-event`，按下时从 DRM 连接器的 DPMS 状态判断屏幕亮灭，在当前活动图形会话中通过 GNOME Shell / Mutter 的 D-Bus 接口执行动作：

- 短按：屏幕亮时熄屏，屏幕灭时点亮。二者调用扩展 `w202-screen-lock@w202ds` 的 `/io/github/w202/ScreenLock` `ScreenOff` / `ScreenOn`；扩展未运行时退回旧路径（`org.gnome.ScreenSaver.SetActive` 升起屏幕盾牌，再由 Mutter `PowerSaveMode` 熄屏 / 亮屏）。
- 按住 1 秒（按键尚未松开即触发）：若按下时屏幕是灭的，先以 `PowerSaveMode 0` 点亮，再调用会话总线 `org.gnome.Shell` 上的 `/io/github/w202/PowerMenu` `io.github.w202.PowerMenu.Open`；随后的松开不再当作短按。没有活动图形会话或扩展未运行（调用失败）时退回旧行为：记录日志后 `systemctl suspend`。
- 唤醒系统的那一次按键交给 GNOME 自己亮屏，不做处理。
- `w202-power-key --test-long-press` 按当前屏幕状态执行一次长按动作后退出，用于不按实体键的验收。

GNOME 自己的电源键动作被 dconf 设为 `nothing`，logind 忽略电源键，二者都不与本程序抢键。GNOME 48 的 `power-button-action` 只有 nothing / suspend / hibernate / interactive；interactive 打开的结束会话对话框每次只给一个确认动作，不能三选一，所以另做菜单。

扩展 `w202-power-menu@w202ds` 复用 GNOME Shell 自身组件，不另画界面：

- 对话框是 Shell 的 `ModalDialog` + `MessageDialogContent`，并沿用 Shell 关机 / 重启对话框的 `end-session-dialog` 样式（宽度一致），外观与系统对话框相同；标题及按钮文字直接取 gnome-shell 自带翻译（“关机菜单”“取消”“挂起”“重启”“关机”）。触摸点按、Tab/方向键切换、回车/空格激活、Esc 取消均由 Shell 对话框提供，初始焦点在“取消”。
- gnome-session 48 的 `Shutdown` / `Reboot` 一律弹出 Shell 的结束会话确认框（60 秒倒计时）；48 版已没有不弹框的 `RequestShutdown` / `RequestReboot`。为避免二次确认，扩展在发起请求前用 `IsInhibited(LOGOUT)` 查询抑制器：无抑制时，确认框出现后立即代为确认（发出 `ConfirmedShutdown` / `ConfirmedReboot`，与用户点确认相同）；有应用阻止注销、确认框类型不符（例如待安装离线更新的“重启并安装”）或框内列出应用时，保留 GNOME 原确认框交给用户处理。
- 锁屏时沿用 GNOME 规则：`SystemActions` 在锁屏状态下不提供关机和重启（gsd 的 interactive 电源键在锁屏上也不弹框），菜单中这两个按钮置灰并提示“解锁后才能关机或重启”，挂起和取消可用。扩展的 `session-modes` 含 `unlock-dialog`，锁屏后仍在运行。
- 打开时若屏幕盾牌处于激活状态（锁屏或熄屏），先按 GNOME 恢复唤醒时的同一路径唤醒锁屏（`ScreenShield._wakeUpScreen()`：撤掉黑色遮罩并发出 `WakeUpScreen`，由 gnome-settings-daemon 点亮屏幕），否则菜单会被锁屏的黑色遮罩盖住。
- 菜单打开时短按电源键熄屏（Mutter `PowerSaveMode` 变为非 0），或屏幕盾牌升起，菜单随之关闭。
- D-Bus 接口：`Open()`、`Close()`、只读属性 `IsOpen`。每次打开、关闭和选择都以 `w202-power-menu@w202ds:` 前缀写入 gnome-shell 日志。

扩展 `w202-screen-lock@w202ds` 在 GNOME Shell 自己的屏幕盾牌上改出手机式的息屏 / 亮屏行为，认证、通知、空闲淡出仍由 GNOME 处理：

- 熄屏时不播放锁屏下滑动画，直接以不带动画的方式升起盾牌并熄屏，熄屏过程中看不到锁屏界面。升起顺序与 GNOME 的 `lock()` 相同：先取得盾牌自己的 `LOCK_SCREEN` 模态抓取，再打开解锁对话框（`UNLOCK_SCREEN`）。顺序反过来时盾牌的抓取压在对话框之上，对话框失去键盘焦点，触摸上划手势也被禁用。
- 亮屏后显示锁屏时钟。未到锁定时间时，触摸上划（或按键、滚轮下滚）即回到之前的界面，不要密码；单点不解锁，防止误触（GNOME 的点按动作在一次没划够的拖动松手时也会触发）。上划没过半且速度不够时锁屏弹回。
- 上划解锁由扩展自己完成滑出动画，动画结束后调用 GNOME 不带动画的 `deactivate(false)`。GNOME 带动画的 `deactivate(true)` 没有任何 GNOME 代码路径使用：它会取消自身的滑出动画，使盾牌停在“正在隐藏”状态，之后再熄屏时盾牌无法重新显示，只剩一个看不见、却抓住全部输入的锁屏压在冻结的画面上（表现为画面不动、触摸无效、鼠标指针拖出残影、画面停在半屏位置）。
- 熄屏时间达到 `org.gnome.desktop.screensaver lock-delay`（默认 15 分钟，0 为立即，`lock-enabled` 关闭则从不）后锁定，此后上划显示密码框。计时用墙上时钟并在亮屏时再比较一次，挂起的时间也计入；设了非 0 的锁定时间时，挂起不再立即锁定。
- 盾牌升起期间任何原因点亮屏幕（gnome-settings-daemon 在插拔电源时、通知、按键）都会撤掉盾牌的黑色遮罩，屏幕不会亮着却全黑。
- 熄屏发生在上划滑出动画途中时，锁屏留在原位；发生在 GNOME 收尾的最后一段动画中时，等盾牌放下后再重新升起。
- 短按电源键熄屏先用 0.3 秒渐暗再关屏，桌面和锁屏上都一样；渐暗途中再按一次取消熄屏。屏幕已被盾牌的黑色遮罩盖住时直接关屏。
- 亮着的锁屏（含密码框）无操作时先用 3 秒渐暗，再关屏：最后一次输入后 30 秒（gnome-settings-daemon 的 `SCREENSAVER_TIMEOUT_BLANK`，它原本直接关屏），被 gnome-settings-daemon 点亮而无输入时为点亮后 15 秒。渐暗途中有输入即恢复亮屏。电源键 / 开盖亮屏不发出 GNOME 的 `WakeUpScreen`，否则 gnome-settings-daemon 15 秒后会不经渐暗直接关屏。桌面空闲时仍是 GNOME 自己的 10 秒渐暗。
- 锁屏背景是壁纸本身加一层 30% 黑色，不用 GNOME 的重度模糊（模糊效果整个移除，不是把半径设 0）。
- 上划过程中显示锁屏下方的会话（窗口组）：unlock-dialog 模式下 GNOME 隐藏了窗口组和桌面背景，锁屏被推开的部分没有任何东西绘制，屏幕上会留下以前帧的横条纹；上划未到位弹回时恢复隐藏。
- 合盖（霍尔开关 `w202_hall`，经 UPower 与 Mutter 的 `lid-is-closed-changed`）立即熄屏、开盖亮到锁屏；合盖期间无论谁点亮屏幕都立即关回。logind 忽略合盖（`50-w202-lid.conf`），不挂起：本机 Wi-Fi 固件不能从挂起恢复。
- 每次熄屏、亮屏、锁定、合盖、开盖都以 `w202-screen-lock@w202ds:` 前缀写入 gnome-shell 日志。

设置程序 `w202-screen-settings`（应用名“屏幕与锁屏”，GTK 4 / libadwaita）直接修改当前用户的标准 GNOME 设置，选中即生效：自动息屏（`org.gnome.desktop.session idle-delay`）、需要密码的时间（`org.gnome.desktop.screensaver lock-enabled` / `lock-delay`）、锁屏上显示通知（`org.gnome.desktop.notifications show-in-lock-screen`）、使用电池时自动挂起（`org.gnome.settings-daemon.plugins.power sleep-inactive-battery-*`）。接通电源时从不自动挂起（`50-w202-no-ac-suspend`），程序中注明了原因。

## 构建输入输出

在本目录执行：

```sh
make OUT="$PWD/output" all
make OUT="$PWD/output" DESTDIR="$PWD/output/rootfs" install
```

输入：本目录源码与 `po/*.po`，需要 GNU gettext 的 `msgfmt`。输出：编译后的 `.mo` 及离线 rootfs 暂存树（扩展装到 `/usr/share/gnome-shell/extensions/w202-power-menu@w202ds/` 与 `w202-screen-lock@w202ds/`）。`install` 只接受显式 `DESTDIR`，不启用服务。

目标系统依赖 GNOME Shell 48、gnome-session、Python 3（设置程序另需 PyGObject、GTK 4 与 libadwaita）、systemd、util-linux（`setpriv`）及 GLib 的 `gdbus`。集成时执行：

```sh
dconf update                                   # 60-w202-power-key：电源键动作 nothing，锁定时间 15 分钟，默认启用两个扩展
systemctl daemon-reload
systemctl enable --now w202-power-key.service
```

新安装的扩展只在 GNOME Shell 启动时被发现，Wayland 下需要重新登录或重启。扩展通过系统 dconf 数据库的 `org.gnome.shell enabled-extensions` 默认值启用；用户若自己修改过扩展列表，以用户值为准，需保证其中仍含这两个扩展。

## 验收

辅助脚本用于不碰平板完成验收：`tests/shell-screenshot.py` 以会话用户身份运行，临时占用 `org.gnome.Screenshot` 名称（GNOME Shell 只为白名单名称截图），调用 `org.gnome.Shell.Screenshot` 保存整屏截图；`tests/uinput-key.py` 以 root 身份通过 uinput 敲一次指定键（1 = Esc，15 = Tab，28 = 回车）；`tests/uinput-touch.py` 以 root 身份创建与面板同尺寸（1200×1920 竖向坐标）的 uinput 触摸屏，做一次直线滑动或点按（`tap X Y`），可选第 6 个参数为手指停在终点的秒数，用于截取上划途中的画面。横屏下“上划”沿面板 X 轴由小到大，例如 `uinput-touch.py 200 960 1000 960 350`。

2026-11-08 在 W202DS（Debian 13，GNOME Shell 48.7，gnome-session 48.0，内核 `6.18.35-w202ds-m23-wifi`）上验证，结果均来自重启后的实机：

- 重启后 `gnome-extensions info w202-power-menu@w202ds` 为已启用 / `ACTIVE`，`gdbus introspect` 可见 `io.github.w202.PowerMenu`（`Open`、`Close`、`IsOpen`）；用户 dconf 中没有扩展列表，生效值来自系统默认。
- `w202-power-key --test-long-press`：未锁屏时打开菜单，截图为“关机菜单”及“取消 / 挂起 / 重启 / 关机”四个按钮，焦点在“取消”；`IsOpen` 为 true，Esc 后为 false，`Close()` 同样可关闭。
- 熄屏未锁时长按：先点亮（DPMS Off → On）再打开菜单。菜单打开时设 `PowerSaveMode 3`：菜单关闭（日志 `closed (screen off)`）。
- `org.gnome.ScreenSaver.Lock` 锁屏并熄屏后长按：屏幕点亮，锁屏上显示菜单，重启 / 关机置灰并提示“解锁后才能关机或重启”，挂起可用；日志 `opened on the lock screen`，Esc 取消。
- 键盘：两次 Tab 后焦点移到“重启”，回车激活。在 `gnome-session-inhibit --inhibit logout` 存在时选择重启，日志为 `logout is inhibited`，GNOME 原确认框列出阻止注销的程序并给出“仍然重启”，Esc 取消后会话回到 running 状态。去掉抑制后选择重启：日志 `end-session dialog confirmed: ConfirmedReboot`，1 秒内 logind `The system will reboot now!`，系统正常重启（无二次确认）。
- 回退：禁用扩展后执行 `--test-long-press`（`systemctl` 替换为记录参数的假程序），日志 `long press: power menu unavailable, suspend`，并调用了 `systemctl suspend`。

手机式锁屏（`w202-screen-lock@w202ds`）同日验证，GNOME Shell 临时以 `--unsafe-mode` 运行以便用 `org.gnome.Shell.Eval` 读出盾牌状态，验收后已恢复：

- 改动前复现：熄屏 → 亮屏 → 解锁走 GNOME 带动画的 `deactivate(true)` 后，盾牌停在“正在隐藏”状态且仍处 `unlock-dialog` 模式、持有输入抓取；再熄屏 / 亮屏后切换概览，DPU 提交帧数为 0，鼠标移动在扫描缓冲中留下一圈指针残影，与用户实机所见一致。
- 改动后：触摸上划 3 次、按键 1 次，每次盾牌都放下（`user` 模式、无抓取），切换概览各产生 15～21 次提交，鼠标移动无残影；单点、向下划、慢速短划都留在锁屏。连续快速熄屏 / 亮屏（0.3～0.6 秒间隔）以及上划滑出途中熄屏之后，盾牌都能正常升起和放下。
- `lock-delay` 设为 5 秒、熄屏 8 秒后亮屏：已锁定，上划显示密码框；`loginctl unlock-session` 后画面正常刷新。熄屏 2 秒后亮屏：未锁定，上划直接回到桌面。
- 设置程序：截图中四项分别显示 5 分钟 / 15 分钟后 / 开 / 30 分钟；逐项选择后读回 GSettings：需要密码“1 分钟后”→ `lock-enabled true`、`lock-delay 60`，“从不”→ `lock-enabled false`；自动息屏“2 分钟”→ `idle-delay 120`；使用电池时“从不”→ `nothing`，“1 小时”→ `suspend` / 3600。

同日第二轮（用户实机观察并确认）：

- 上划锁屏：改动前手指停在半路时截屏，锁屏被推开的下半部分是横条纹（与用户照片一致）；改动后为会话画面，用户确认不再出现条纹。
- 渐暗：用户确认桌面与锁屏上短按电源键都有渐暗；锁屏无操作约 27 秒开始渐暗、30 秒关屏（日志 `idle on the lock screen`），渐暗途中触摸恢复亮屏。
- 合盖：用户多次合盖 / 开盖，日志每次合盖立即熄屏、开盖亮屏；合盖时折叠键盘按键点亮的屏幕立即关回（`cover closed: screen kept off`）。

待用户实机确认：按住电源键 1 秒弹出菜单、松开后屏幕不熄灭；触摸点按挂起 / 重启 / 关机 / 取消；熄屏及锁屏状态下长按；菜单打开时短按电源键熄屏；真实手指上划解锁的手感。关机与挂起两项按钮未在本轮自动验收中实际执行（挂起后需按电源键唤醒）。
