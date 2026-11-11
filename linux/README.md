# Linux experimental backend

This is a native C++26 HID++ backend using HIDAPI's hidraw transport and Linux
uinput, with a Qt 6 Widgets interface. It is independent third-party software,
not endorsed by Logitech. Linux physical-device behavior is **not yet verified**.
CI compilation and offline tests do not establish MX Master 3S hardware support.

## Build

Dependencies: CMake 3.30+, a C++26-capable compiler (CI uses Clang 19), pkg-config,
libhidapi-dev, GLib/GIO, libwayland and Qt 6 Widgets/DBus development files. For Ubuntu:

```sh
sudo apt install clang-19 libhidapi-dev libglib2.0-dev libwayland-dev pkg-config qt6-base-dev qt6-wayland libx11-dev libxtst-dev xvfb xauth openbox
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
xvfb-run -a sh tests/run-linux-desktop-tests.sh build
./build/logi-options-open
```

Use `-DLOGI_BUILD_GUI=OFF` to omit the settings window. The KDE desktop bridge
still uses Qt Core/Gui/DBus, separately from the device daemon. No browser runtime
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
`stop` waits for input restoration and reports restoration failures. `reload [file]`
waits for device and desktop configuration to apply; failures preserve the previous
live configuration or clear readiness if rollback fails. A different path requires restarting the daemon.

Supported commands: `list`, `status`, `status-json`, `hosts`, `hosts-json`, `desktop-status`,
`agent-status` (alias `agent-json`), `set NAME VALUE`, `switch-host N`,
`recover-input`, `check-config`, `run`, `reload`, `stop`, `disclaimer`,
`accept-disclaimer`, `check-disclaimer`, `check-actions FILE`, `play-actions FILE`,
`preview-ring CONFIG`. `agent-status.native` describes the daemon process;
`ready` and `connected` describe its device state. Setting writes are read back.
Host switching only sends the request; verify reception on the destination.

Settings reuse the Windows HID++ implementation: DPI, SmartShift threshold,
ratchet, high-resolution scrolling, wheel inversion and thumb inversion, subject
to device features. A single daemon owns HID and serializes CLI/API requests over
local IPC. Thumb inversion is applied after releasing diversion and then the
configuration is reapplied. Host switching enters reconnecting state. Status
contains cached device values; settings written by this daemon update that cache
after readback. Recovery stops remapping before restoring standard input.

`[global]` is the fallback profile. X11 application sections match WM_CLASS or
the foreground executable basename, case-insensitively. Native Wayland profiles
match compositor-provided application/desktop IDs, e.g. `[org.mozilla.firefox]`.
`desktop-status` reports live desktop capabilities without opening the mouse.
For example, `[firefox]`
can override only `thumb.left=Ctrl+Shift+Tab` while inheriting the other mappings.
The GUI provides application selection and structured Smart Action editing.
Buttons use hexadecimal control IDs;
`.left`, `.right`, `.up`, `.down` enable raw-motion gestures on a button, which
also requires a tap mapping. `thumb.left`/`thumb.right` map the horizontal wheel,
and `thumb.speed=10..200` scales its resolution. Example actions include
`Ctrl+C`, `Super+Tab`, `MouseBack`, `VolumeUp`, `WheelMode`, `ScrollLeft`, and
`Ctrl+C;Wait(100);Ctrl+V`. `Open(file-or-URL)` invokes xdg-open without a shell.
Desktop shortcuts differ by compositor; edit the defaults for your desktop.

`[hotkeys]` registers global triggers, for example `Ctrl+Shift+F10=Ctrl+C;Wait(100);Ctrl+V`.
Shortcuts mapped by XKB to virtual-terminal switching (commonly Ctrl+Alt+F1–F12)
are rejected with their actual reserved keysym, rather than silently registering
a trigger the application cannot receive. Other conflicts fail explicitly;
generated actions temporarily suspend this process's
grabs to avoid recursively triggering themselves. `Text(hello)` and UTF-16 encoded
`TextHex(...)` paste UTF-8 through the desktop clipboard; **this replaces CLIPBOARD**
and requires a target application that supports Ctrl+V. The daemon services
clipboard requests asynchronously; an unaccepted paste reports an action error.

`[ring]` and `[ring/Media]` define native Qt menus, e.g. `Media=Folder(Media)`.
Use the GUI's Create default ring button, then open the ring or bind `Ring` to a
mouse control. Mouse/number/arrow/Enter selection and Escape/back are supported.

`play-actions FILE` uses one `[hotkeys]` entry as its action container. A running
daemon returns `accepted:true, action_id:N` only after enqueueing, not after
execution. Inspect `pending_actions`, `last_completed_action`, and
`last_action_error`; a later action can finish before an earlier waiting action.
Standalone playback returns `completed:true` only after all steps finish.

Single mouse-button mappings preserve down/up for dragging, including correct
release after a profile switch. Reload, disconnect and shutdown release held
virtual buttons. The daemon retries a lost device and restores its saved diversion
state before reapplying mappings. Crash/forced-exit recovery still uses `recover-input`.
Stopping while the physical device is offline cannot restore that offline device;
after reconnecting, use `recover-input` if its controls remain diverted. Saved
restoration state is retained in memory only while the daemon stays running.

## Native Wayland integration

This development branch adds compositor-native desktop bridges. Actual GNOME/KDE
acceptance is a release gate; implementation or compilation alone is not a pass.

- GNOME Shell 46: install and enable the optional [Shell extension](gnome/README.md).
- KDE Plasma 5.27/KWin Wayland: run the optional [desktop provider](kde/README.md).
  Complete shortcut-release detection additionally needs read access to the active
  keyboard's evdev state. Missing access is reported, not silently ignored.

The bridge reports native foreground IDs, owns only this daemon's requested
shortcuts, and serves immutable clipboard data. A shortcut conflict retains the
previous registrations; closing the daemon releases its sender's resources.
Keyboard actions continue through production uinput. No Xwayland foreground or
clipboard fallback is used. Other compositors, or sessions without the required
bridge, reject unsupported profiles/hotkeys/text explicitly; basic device mappings
remain independent of desktop integration.

Clipboard data-read acknowledgements order consecutive text actions. They do not
prove the intended application inserted the text: clipboard managers can also
read a selection. Target Ctrl+V support and actual receiver behavior matter.

Flow, vendor
pairing/firmware updates and the full official Smart Actions template/plugin
catalog are not implemented. Left/right physical buttons and the main wheel
are not grabbed or diverted by the mapping daemon.

## Validation boundary

The desktop test script runs actual X11 focus, key events, global grabs, UTF-8
clipboard transfers and Qt mouse/keyboard interactions under Xvfb/Openbox. GUI
tests run the real CLI validator and verify persisted configuration/error handling.
Separate processes exercise real CLI/HTTP/MCP against the production daemon loop
with explicitly simulated device/input adapters, including reconnect and failures.

The GNOME and KDE runners start isolated real Wayland compositors, with native Qt
clients. They verify application IDs/profile switching, actual global-shortcut
release and conflict handling, native Unicode/consecutive pastes and GUI/Ring
interaction. Production uinput writes real kernel events; because the CI desktop
is nested/virtual, a test-only evdev transport feeds those observed events into
the compositor's native input API. This transport is not installed in the product
and does not substitute a fake Qt window or Xwayland client. Logs and receiver
screenshots are retained as CI artifacts, including on failure.

`sudo modprobe uinput` followed by `sudo ctest --test-dir build -R linux-uinput-kernel -V`
tests the production uinput implementation against actual kernel evdev events.
Only this test needs elevated input access in an ephemeral test environment; the
application itself uses the normal permissions setup above. Missing kernel access
is a skip, not successful input validation. CI treats missing module setup as failure.

None of these validates receiver/Bluetooth transport, physical scroll direction,
real device reconnection/restoration, other compositors or Linux mouse memory use.
Those require a Linux host with the physical mouse. No such host is available for
this development session; physical-device acceptance remains incomplete.

Transport/input API references: [HIDAPI](https://libusb.info/hidapi/group__API.html)
and [Linux uinput documentation](https://docs.kernel.org/input/uinput.html).
