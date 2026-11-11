# Linux experimental backend

This is a native C++26 HID++ backend using HIDAPI's hidraw transport and Linux
uinput, with a Qt 6 Widgets interface. It is independent third-party software,
not endorsed by Logitech. Linux physical-device behavior is **not yet verified**.
CI compilation and offline tests do not establish MX Master 3S hardware support.

## Build

Dependencies: CMake 3.30+, a C++26-capable compiler (CI uses Clang 19), pkg-config,
libhidapi-dev, and Qt 6 Widgets development files. For Ubuntu:

```sh
sudo apt install clang-19 libhidapi-dev pkg-config qt6-base-dev
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/logi-options-open
```

Use `-DLOGI_BUILD_GUI=OFF` for a small CLI/daemon-only build. No browser runtime
is required. The daemon reads HID input with a bounded blocking wait and does
not run the GUI. Actual memory consumption has not been measured on Linux.

## Permissions and startup

The GUI and daemon run as the logged-in user, never as root. The kernel uinput
module and access to the Logitech hidraw device and `/dev/uinput` are needed.
The supplied rule is opt-in; it grants a dedicated group access, not every user.
Only trusted users should join this group because uinput permits synthetic input.

```sh
sudo groupadd -f logioptions
sudo usermod -aG logioptions "$USER"
sudo modprobe uinput
sudo install -m 0644 linux/70-logi-options-open.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw
sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput
```

Log out and back in for group membership to apply. Persisting module loading is
optional. No services are enabled by installation. `logi-options-open.service`
is an optional **user** service template for an installation in `/usr/local`;
adjust ExecStart for another prefix. Complete first-use acknowledgement and
create your mappings before enabling it. Stop Solaar/logid yourself before using
this daemon; it refuses known competing managers and never kills them.

## Configuration and commands

The GUI creates mappings from `default-linux.ini`. For CLI-only use:

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/logi-options-open"
cp linux/default-linux.ini "${XDG_CONFIG_HOME:-$HOME/.config}/logi-options-open/mappings.ini"
./build/logi-cli disclaimer
./build/logi-cli accept-disclaimer
./build/logi-cli check-config
./build/logi-cli run
```

The disclaimer is acknowledged **once per user**, shared by GUI and CLI.
Read-only commands and input recovery remain available without acknowledgement.
`LOGI_OPTIONS_OPEN_CONFIG` overrides the mapping path. Otherwise XDG_CONFIG_HOME
(or `~/.config`) is used. Run stays in the foreground; the GUI starts it detached.
`stop` requests graceful shutdown and waits for restoration. `reload [file]`
validates then requests a reload of the running config path; inspect the daemon
log for acknowledgement/failure. A different path requires restarting the daemon.

Supported commands: `list`, `status`, `status-json`, `hosts`, `hosts-json`,
`agent-status` (alias `agent-json`), `set NAME VALUE`, `switch-host N`,
`recover-input`, `check-config`, `run`, `reload`, `stop`, `disclaimer`,
`accept-disclaimer`, `check-disclaimer`. `agent-status.native` describes the
daemon process, not verified hardware readiness. Setting writes are read back.
Host switching only sends the request; verify reception on the destination.

Settings reuse the Windows HID++ implementation: DPI, SmartShift threshold,
ratchet, high-resolution scrolling, wheel inversion and thumb inversion, subject
to device features. Stop the daemon before changing thumb inversion/host or
running input recovery. Concurrent status/settings requests while the daemon
runs require physical-device validation and may fail; they are not proof of
correct device ownership or delivery.

Only `[global]` mappings are implemented. Buttons use hexadecimal control IDs;
`.left`, `.right`, `.up`, `.down` enable raw-motion gestures on a button, which
also requires a tap mapping. `thumb.left`/`thumb.right` map the horizontal wheel,
and `thumb.speed=10..200` scales its resolution. Example actions include
`Ctrl+C`, `Super+Tab`, `MouseBack`, `VolumeUp`, `WheelMode`, `ScrollLeft`, and
`Ctrl+C;Wait(100);Ctrl+V`. `Open(file-or-URL)` invokes xdg-open without a shell.
Desktop shortcuts differ by compositor; edit the defaults for your desktop.

Unsupported features fail validation explicitly: per-app profiles, global
trigger hotkeys, Ring overlay, text injection and Windows-specific actions.
Flow, vendor pairing/firmware updates, Smart Actions templates and auto-reconnect
are not implemented. Mouse click actions currently synthesize a click, not a
held drag. The daemon exits on unplug or input errors. Stop before disconnecting
when possible; reconnect and use `recover-input` if a crash or forced termination
left device controls diverted. Left/right physical buttons and the main wheel
are not grabbed or diverted by the mapping daemon.

## Validation boundary

Offline CTests cover strict config parsing, unsupported input, gesture thresholds,
fractional/reversed wheel accumulation, default mappings and packet bounds.
They do not cover receiver/Bluetooth transport, compositor shortcuts, actual
scroll direction, sleep/reconnect, hardware restoration or Linux memory use.
Those require a Linux host with the physical mouse. No such test was performed
during the Windows-hosted initial port.

Transport/input API references: [HIDAPI](https://libusb.info/hidapi/group__API.html)
and [Linux uinput documentation](https://docs.kernel.org/input/uinput.html).
