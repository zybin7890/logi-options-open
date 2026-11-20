# Windows 原生候选（Win32 / C++26）

当前开发优先 Windows。此目录保留旧版 Win32 界面及最新后台候选，独立于根目录的 Qt 界面构建；Linux 本轮没有改动。

**这是独立第三方、不成熟的实验性软件，与 Logitech 无隶属关系。此提交是源码候选，不是已通过鼠标实测的稳定发行版。** 首次使用复用根目录的免责声明及本机确认记录，只提示一次；取消不会启动后台。

## Windows 接管更新

`logi-native.exe` 同时提供设置窗口、后台和 CLI。双击程序完成首次使用确认。确认后可以用 `logi-native.exe --tray` 在后台运行。命令行用户也可以先运行 `logi-native.exe disclaimer` 阅读须知，再运行 `logi-native.exe accept-disclaimer`，确认记录在当前 Windows 账户中。

后台接管时跳过只用于展示的固件和序列号查询。一次无线读取超时会在当前连接中重试，期间继续接收按键。平滑滚动输出被前台应用拒绝时，其他映射继续工作。`agent-status` 的就绪状态在输入循环初始化后发布。

在一台通过 Bolt 连接的 MX Master 3S 上，关闭官方后台，以相同配置各启动一次，托盘进程启动到就绪的观测时间由 3.08 秒降至 1.02 秒。侧键、主滚轮、拇指轮与拇指手势通过人工操作验证。鼠标电源重开、登录后接管耗时与长期稳定性仍待验证。

## 已包含的修改

- DPI、五项滚轮设置通过后台读写和返回确认，减少 GUI 与后台争用设备；设置和映射重载带实例、请求及版本检查。
- 指针与滚轮页面新增 DPI 快捷预设（`800 / 1600 / 3200 / 5000`）与 Windows “提高指针精确度”（`SPI_GETMOUSE` / `SPI_SETMOUSE`）开关。
- 手势会话在按下时捕获前台窗口、进程和方向映射；焦点变化、配置重载、设备通知丢失及停止会取消会话，包含连续媒体手势。
- 取消后等待物理释放，避免补发点击；部分输入注入失败时尝试清理本次已按下的键，保留已有物理修饰键。
- 集成 Flow 跨屏 LAN 发现与 BCrypt HMAC-SHA256 剪贴板/边缘同步、`HID++ 0x0003` 设备序列号/固件信息、Bolt 接收器扫描/配对锁与系统蓝牙跳转、低电量/锁键/麦克风状态 OSD 通知。
- 支持 `.lp5` / `.lp4` / `.ini` 动作环配置导入导出、界面设置叠加面板与槽位拖拽、`简体中文` / `English` 完整双语切换、深/浅/跟随系统主题，以及通过系统 `winsqlite3.dll` 只读导入本地 `LogiOptionsPlus\settings.db`。
- 支持系统托盘图标（`--tray` 静默启动、关闭最小化到托盘、`FindWindowW` 单实例唤醒已有窗口、`TaskbarCreated` 重建及右键菜单）与设置页 `HKCU\...\Run\LogiNative` 开机自启开关（默认不自动写入开机启动）。
- Smart Actions（支持 `DateTime`、`Widget`、`HostSwitch`、`ZoomReset`、`RotateLeft/Right`、`GestureZoomRotate` 等）、Ring、CLI 及 Python API/MCP 代码均在本目录同步更新，支持范围不等同于原版全部功能（不含固件更新）。

## CLI

```powershell
.\logi-native.exe status-json
.\logi-native.exe hosts
.\logi-native.exe agent-status
.\logi-native.exe check-config .\mappings.ini
.\logi-native.exe run-preview .\mappings.ini
# 在另一个 PowerShell 窗口中执行
.\logi-native.exe set dpi 3900
.\logi-native.exe stop
```

运行 `logi-native.exe help` 查看完整命令。停止设备服务用 `stop` 恢复标准按键与滚轮报告。

若退出鼠标软件后滚轮或侧键失效，先退出运行中的鼠标后台，再执行 `logi-native.exe recover-input`。它清除滚轮、侧键和手势键的 HID++ 转发状态，并逐项读回验证；不改 DPI、滚动方向或配置文件。启动服务时也会先清理遗留转发状态，防止主滚轮继续转发到已退出的软件。

## 本地 API

```powershell
python .\bridge.py serve --port 47126
# 另一个 PowerShell 窗口中：将 <token> 换成启动时打印的随机令牌
$h = @{ Authorization = 'Bearer <token>' }
Invoke-RestMethod http://127.0.0.1:47126/v1/status -Headers $h
```

只监听 `127.0.0.1`。令牌可由当前进程环境变量 `LOGINATIVE_API_TOKEN` 指定，无需账号。地址请使用启动时显示的地址。

| 方法 | 路径 | JSON 参数 |
|---|---|---|
| GET | `/v1/status`、`/v1/hosts`、`/v1/agent` | — |
| GET | `/v1/config`、`/v1/log` | — |
| PUT | `/v1/setting` | `{"setting":"dpi","value":3900}` |
| PUT | `/v1/config` | `{"text":"…完整 INI…"}`，通过原生校验才保存 |
| POST | `/v1/agent/start` | `{"takeover":true}`，返回进程 ID，进度看日志 |
| POST | `/v1/agent/stop` | `{}` |
| POST | `/v1/switch-host` | `{"slot":2}`，槽位必须已配对 |
| POST | `/v1/ring/preview` | `{}`，打开原生菜单预览 |
| POST | `/v1/actions/preview` | `{"sequence":"Wait(400);Text(Hello)"}`，三秒后执行 |

## MCP stdio

在 MCP 客户端中设为本地 stdio 服务器。将路径替换为解压后的实际路径。

```json
{
  "mcpServers": {
    "loginative": {
      "command": "python",
      "args": ["C:/path/to/LogiNative-MX3S/bridge.py", "mcp"]
    }
  }
}
```

默认只开放 `mouse_status`、`easy_switch_slots`、`agent_status`、`read_mappings`。仅在需要执行更改时，在 `args` 末尾加 `"--write"`，即可开放设备写入、保存配置、启动和停止后台、已配对槽位切换。

## 构建与离线验证

要求 Windows、CMake 4.1+、支持 C++26 的 MinGW-w64 编译器及 Ninja。以下命令从仓库根目录运行，编译器工具目录应在 PATH 中：

```powershell
cmake -S windows-native -B build-win32 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win32 --parallel 4
ctest --test-dir build-win32 --output-on-failure
.\build-win32\logi-native.exe check-config .\build-win32\mappings.ini
```

CTest 运行实际生产路径与隔离替身测试，覆盖设置持久化（含 `close_to_tray`）、`.lp5` 压缩包、`HID++ 0x0003` 解析、Flow `127.0.0.1` 环回传输、`options_plus_import` JSON/DPI 解析、连续媒体、平滑滚轮与重连/恢复逻辑。编译及单测通过不代表全部多机/休眠/长期场景已验收。

构建使用仓库自制示意图，不分发原版提取图片、字体、二进制或逆向转储。因此公开源码构建的图片与本机旧版截图不同。缺少原版图标字体时使用已有字体回退。

## 运行行为与验证边界

打开 `logi-native.exe` 并首次确认提示后，GUI 会尝试启动本目录后台并暂停原版 Options+ GUI/Agent；关闭设置窗口后后台继续运行。`logi-native.exe stop` 停止试用。不要同时运行本目录候选和根目录版本接管同一鼠标。构建或 CTest 不会执行此接管流程。

本轮已在用户授权下独立运行候选并更新本机桌面程序；首次发现官方 Updater 重新拉起 Agent 后，按用户要求禁用官方自启动，再完成真实鼠标和参数检查。预设列表与方向自定义也通过实际 Win32 窗口点击验证。详情见 [实测记录](LIVE-VALIDATION.md)。没有创建稳定 Release，也不把本机的启动项修改作为软件默认行为。

## 连续媒体手势

在手势预设中选择“媒体控制（连续）”：点击播放/暂停，按住上下移动调节音量，左右移动切曲目。选择后自动保存，不需要“应用”。原“媒体控制”保留松开执行方向动作的行为，不自动迁移用户配置。

连续模式先识别轴，再以新的移动量触发方向动作；起始达到阈值只启动，不立刻输出。静止不重复；每个动作后重新识别，单包最多一个动作。横向动作后 400 毫秒内丢弃移动量（含纵向），到达 400 毫秒才能重新开始。释放已经启动过的手势不再补发点击。切窗、重载、DPI 更新、丢包和停止会取消并清理状态。

配置用 `GestureMedia` 作为点击项，四个方向必须分别为 `PreviousTrack`、`NextTrack`、`VolumeUp`、`VolumeDown`。不能用于宏、热键、Ring 或滚轮。修改方向时界面自动回到普通自定义手势；手工配置若继承了连续模式却覆盖方向，会被拒绝。

原版卡片的连续标记、媒体方向、无静止重复及横向 400 毫秒参数已核对；原版的输入归一化和完整结束链路未闭合。本实现沿用本项目 DPI 对原始移动量的阈值 `max(20,dpi/12)`，起始主轴要求严格占优，动作量须严格超过阈值。释放规则和识别期间的轴固定是本地策略，**不声称已达到原版手感或完整语义等价**。

尚未完成或未验证：

- 连续媒体手势已实现，本机用户确认功能正常；原版完整手感和语义仍待校准。
- 原版完整连续轴、方向锁定、自动重复/暂停及专用 Alt 导航语义未补齐。
- 本机侧键、两个滚轮、拇指手势、DPI 和五项参数已实测；进程重启加载已通过，整机重启/登录、休眠重连和长期稳定性仍需验收。
- UI 外观、动画、缩放和真实点击尚未对这份公开构建验收。
- Linux、Flow、配对、固件更新及完整官方插件兼容不属于本次 Windows 候选验收。

原始开发源码复制至本目录；公开适配仅更换构建资源来源、复用首次免责声明，并添加可复现的离线测试。根目录已有 Qt/Linux 实现保留不变。

完整目标和逐项状态见 [Windows 替代验收表](REPLACEMENT-ACCEPTANCE.md)。
