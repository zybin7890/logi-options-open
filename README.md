# Logi Options Open

**独立第三方、实验性、尚不成熟。与 Logitech / 罗技无隶属关系，未获其认可或支持。**

原生鼠标设置与改键工具，面向支持 HID++ 2.0 的 Logitech 鼠标，首先适配 MX Master 3S。Windows 使用 C++26 / Win32，Linux 使用 C++26 / hidapi / uinput 与 Qt6 Widgets；界面不使用浏览器。没有账号、广告、营销或遥测。

> 功能不完整，错误可能导致自定义按键、滚轮或手势暂时失效。免责声明只需首次确认一次，GUI 与 CLI 共用本机记录，重启和升级不重复显示；拒绝则退出，不接管设备。软件不提供担保。请保留停止后台和恢复标准输入的方法。

## 平台状态

| 功能 | Windows | Linux（实验性） |
|---|---|---|
| 原生设置界面 | Win32 | Qt6 Widgets |
| 电量、DPI、SmartShift、滚轮参数 | 已实现；MX Master 3S + Bolt 本机验证 | 已移植；需要 Linux 实机验收 |
| 全局按键、拇指滚轮、四向手势 | 已实现；本机验证 | hidapi + uinput 后端；需要实机验收 |
| 应用专属配置、Smart Actions、Actions Ring | 已实现部分功能 | 尚未支持；不会静默当作成功 |
| HTTP API / MCP stdio / CLI | 已提供 | 已适配支持的 Linux 命令 |
| Flow、配对、固件更新、完整官方插件兼容 | 未实现 | 未实现 |

Windows 设备验证来自本项目开发过程；不意味着所有 Logitech 设备均兼容。Linux 的 CI/离线测试不等于鼠标硬件测试。Wayland 可使用 uinput 注入，但桌面快捷键取决于桌面环境；应用识别和全局热键监听不在当前 Linux 支持范围内。不要同时让 Solaar、logid 和本项目修改同一设备。

## Windows

使用发行包，解压到可写目录后运行 `logi-native.exe`。先阅读首次使用提示，确认后才会启动后台并临时暂停原版 Options+ 界面和 Agent。关闭设置窗口后，后台继续运行；在设置中停止试用或执行 `logi-cli.exe stop`，恢复由本项目暂停的原版 Agent。不会禁用原版服务或安装开机启动。

配置在程序旁的 `mappings.ini`。普通设置立即写入/热更新，无需点击“应用”。如需复用旧配置，自行复制其内容；仓库和发行包仅带通用示例，未包含开发者个人应用路径。

```powershell
.\logi-cli.exe disclaimer
.\logi-cli.exe accept-disclaimer
.\logi-cli.exe status-json
.\logi-cli.exe run-preview .\mappings.ini
# 另一个终端：
.\logi-cli.exe stop
```

停止所有改键后台后，可用 `logi-cli.exe recover-input` 恢复标准按钮/滚轮报告。恢复命令无需再次确认免责声明。

## Linux

依赖：支持 C++26 的编译器、CMake 3.30+（建议 4.1）、Ninja、hidapi-hidraw 开发包；界面需要 Qt6 Widgets。Ubuntu 24.04 示例：

```sh
sudo apt install clang-19 ninja-build libhidapi-dev qt6-base-dev
python3 -m pip install --user 'cmake>=4.1,<4.2'
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/logi-options-open
```

仅构建后台/CLI：配置时加 `-DLOGI_BUILD_GUI=OFF`。安装可以使用 `cmake --install build --prefix ~/.local`；使用系统前缀的安装和设备访问规则需要自行进行管理员授权，不由 GUI 提权。

Linux 默认用户配置：`$XDG_CONFIG_HOME/logi-options-open/mappings.ini`，未设置时为 `~/.config/logi-options-open/mappings.ini`。可用 `LOGI_OPTIONS_OPEN_CONFIG` 指定另一路径。首次 GUI 启动会从 `default-linux.ini` 生成配置；命令行使用前可从仓库 Linux 目录复制示例。

```sh
./build/logi-cli disclaimer
./build/logi-cli accept-disclaimer
./build/logi-cli check-config linux/default-linux.ini
./build/logi-cli status-json
./build/logi-cli run linux/default-linux.ini
# 另一个终端：
./build/logi-cli stop
```

Linux 需要读写对应的 Logitech hidraw 设备及 `/dev/uinput`。参考 [Linux 说明](linux/README.md) 的设备权限和功能边界；不要用 `chmod 666` 开放所有输入设备。程序不自动提权，不自动终止其他鼠标软件。

## 本地 HTTP API 与 MCP

Python 3.10+ 标准库，无需 pip 运行依赖。Linux 的 bridge 从同目录或 PATH 查找 `logi-cli`；也可以设置 `LOGI_OPTIONS_OPEN_CLI`。从源码树启动时可先设置该变量指向 `build/logi-cli`。

```sh
python3 bridge.py serve --port 47126
python3 bridge.py mcp
```

HTTP 只监听 `127.0.0.1`，需要启动时输出的随机 Bearer token。MCP 默认只读，添加 `--write` 才暴露修改工具。首次免责声明必须先在 GUI 或 CLI 本地确认，API 不替用户确认。

| 接口 | 用途 |
|---|---|
| GET `/v1/status`、`/v1/hosts`、`/v1/agent` | 状态 |
| GET `/v1/config`、`/v1/log` | 用户配置/后台日志 |
| PUT `/v1/setting` | `{"setting":"dpi","value":1600}` |
| PUT `/v1/config` | `{"text":"完整 INI"}`，校验后保存并请求热更新 |
| POST `/v1/agent/start` | `{"takeover":true}`，Windows 临时接管原版；Linux 不关闭其他软件 |
| POST `/v1/agent/stop` | 停止后台并恢复本次转发 |
| POST `/v1/switch-host` | `{"slot":2}`，需要已配对且停止改键 |

Linux 未实现的 Ring / 动作预览返回明确错误。首次确认记录仅存本机用户配置目录，不上传数据；只有删除该记录或换用另一个系统账户才需要重新确认。

## 构建与验证

Windows：MinGW-w64 GCC 支持 `-std=c++26`，CMake 4.1+，Ninja：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cd build && cpack
```

CI 分别构建 Windows 和 Linux，并执行无需设备的测试。Linux GUI 使用 Qt offscreen 验证；它不访问真实鼠标，也不证明所有桌面/显示器上的布局正确。

## 资源与许可

代码与本项目原创示意图以 MIT 许可开源，见 [LICENSE](LICENSE)。鼠标示意图、应用占位图标可通过 `python3 tools/generate-assets.py` 重建。没有分发原版 Options+ 可执行文件、图片、专有字体、反编译文件、个人配置或访问凭据。依赖和商标说明见 [THIRD_PARTY.md](THIRD_PARTY.md)。

**Independent third-party project. Not affiliated with Logitech. Immature experimental software; no warranty. Windows device validation and Linux software-only validation are distinct.**
