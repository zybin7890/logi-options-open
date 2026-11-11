# KDE Plasma native Wayland provider

This optional companion runs in the current user's session. It provides the
`org.logi.OptionsOpen.Desktop1` session-bus interface to the native C++ daemon.
It does not start Xwayland, change compositor settings, steal another application's
shortcuts, install an autostart entry, or grant input-device permissions.

## Start and stop

Build/install the `logi-kde-provider` target, then in a Plasma **Wayland** session:

```sh
logi-kde-provider
```

Leave that terminal running while using Logi Options Open. The normal GUI/daemon
detects the provider via D-Bus. Stop it with Ctrl+C after stopping the mouse
daemon. Closing a daemon's D-Bus connection releases only that daemon's shortcut
registrations and its active clipboard selection. Stopping the provider also
unloads its uniquely named temporary KWin foreground script. No user KWin script
or shortcut component is removed.

The provider targets Plasma 5.27's public KWin scripting and KGlobalAccel D-Bus
interfaces. The script also recognizes Plasma 6's `activeWindow/windowActivated`
names. A successful build alone does not establish support for a Plasma version;
see the release validation results for the actual tested versions and sessions.

## Keyboard-state permission

Foreground matching and clipboard support do not require evdev access. Global
shortcuts additionally require **read access to the session's keyboard event
devices** under `/dev/input/event*`, either through the session's logind ACLs or
an input-access group explicitly configured by the machine's administrator.
The provider checks the actual permissions and reports `shortcuts=false` with
`shortcut_reason` if any keyboard's state cannot be queried. Merely having
`/dev/uinput` write permission does not imply keyboard read permission.

There is intentionally no bundled rule granting everyone access to keyboards.
The provider uses `EVIOCGKEY` to inspect the current key bitmap while a shortcut
is pending; it does not read, save or send an event stream. It waits for the main
key and both sides of Ctrl/Alt/Shift/Meta to be released before delivering the
trigger. New or removed keyboards are discovered during operation.

This distinction matters on Plasma 5: KGlobalAccel's `globalShortcutReleased`
signal can be emitted on the first released key, while other trigger keys remain
held. The provider does not treat that signal or an elapsed delay as full release.

## Clipboard semantics

Text uses KWin's native `zwlr_data_control_manager_v1` protocol. `SetClipboard`
returns a serial; the daemon sends Ctrl+V through its normal uinput path.
`ClipboardConsumed` acknowledges that a recipient requested the UTF-8 MIME payload
and the provider completed its write to the Wayland data-source FD. The daemon
waits for that acknowledgement before the next text action. The clipboard contents are replaced.
This acknowledgement proves transfer, not that a specific target document has
accepted or saved the text. The target still needs to support ordinary paste.

If another application takes clipboard ownership, releasing our sender does not
clear that application's clipboard. The provider removes only its own selection.

## CI transport and evidence

`tests/run-kde-wayland.sh` starts a private D-Bus session and a real
`kwin_wayland --virtual --no-lockscreen` compositor, with `DISPLAY` unset. It uses
the shared Qt Wayland test clients to verify native app IDs, focus changes,
shortcuts, conflict rejection, owner cleanup and UTF-8 paste.

Virtual KWin does not have a physical libinput seat. The **test-only**
`logi-kde-input-forwarder` reads events emitted by the production uinput device
named `Logi Options Open virtual input` and forwards them into KWin's fake-input
protocol. Thus kernel uinput/evdev, the provider and native Wayland clients are
real; the compositor transport is a test adapter, not production hardware input.
The test adapter is not installed as a user-facing input backend. No MX Master 3S
hardware is implied by these tests.

`LOGI_TEST_ALLOW_DEVICE_ACL=1` allows the isolated CI runner to grant access only
to those named test devices. It does not change permissions on physical keyboards.
The evidence includes compositor/provider logs, event transport counts and a
provider RSS/CPU snapshot. That snapshot is not a full daemon performance test.

## Protocol and API sources

The provider implementation and foreground script are project code. Protocol
definitions retain their own upstream notices:

- `protocols/wlr-data-control-unstable-v1.xml`: Simon Ser and Ivan Molodetskikh,
  permissive notice embedded in the file; [upstream protocol](https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/blob/master/unstable/wlr-data-control-unstable-v1.xml).
- `protocols/fake-input.xml`: Martin Gräßlin, LGPL-2.1-or-later, used by the test
  adapter; [KDE upstream protocol](https://invent.kde.org/libraries/plasma-wayland-protocols/-/blob/master/src/protocols/fake-input.xml).
- [KWin scripting API](https://develop.kde.org/docs/plasma/kwin/api/),
  [KGlobalAccel D-Bus interface](https://invent.kde.org/frameworks/kglobalaccel/-/blob/v5.115.0/src/org.kde.KGlobalAccel.xml).
