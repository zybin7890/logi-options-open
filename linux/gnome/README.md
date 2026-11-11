# Optional native GNOME Wayland bridge

This is third-party, immature experimental software, unaffiliated with Logitech.
The extension runs inside GNOME Shell. It can read the focused application ID,
reserve explicitly requested global shortcuts, and replace clipboard text.
It is never installed or enabled automatically by Logi Options Open.

The current extension targets **GNOME Shell 46** (Ubuntu 24.04). Other Shell
versions are not enabled in metadata until their compositor behavior is tested.
There is no Xwayland fallback for native Wayland desktop integration.

## Install as your normal desktop user

From the source tree or the installed `share/logi-options-open/gnome` directory:

```sh
bash install.sh --install
bash install.sh --enable
logi-cli desktop-status
```

No root access is needed for the extension. If GNOME says the new extension is
unknown, log out and back in once, then run `--enable`. It is copied only to
`$XDG_DATA_HOME/gnome-shell/extensions/logi-options-open@zybin7890.github.io`
(default `~/.local/share/...`). The application checks the live D-Bus service;
an installed directory alone is not treated as a working compositor connection.
Mouse hidraw/uinput permissions are separate from extension installation.

Disable using the GNOME Extensions application or:

```sh
bash install.sh --disable
```

Disabling or losing the daemon's D-Bus connection releases its shortcuts and
its clipboard source. Existing GNOME/user shortcuts are never replaced: a
conflicting registration fails and the previous successful registration remains.
When a trigger is held, the extension briefly captures keyboard release events,
then restores focus before dispatch. Escape cancels a pending trigger.

## Bridge protocol

Session bus name/interface `org.logi.OptionsOpen.Desktop1`, object path
`/org/logi/OptionsOpen/Desktop1`, protocol version `1`:

| Method | Input | Result |
| --- | --- | --- |
| `GetCapabilities` | — | `a{sv}`: protocol, desktop, native_wayland, foreground, shortcuts, clipboard, clipboard_ack |
| `GetForeground` | — | `as` of normalized native app IDs / resolved desktop IDs |
| `RegisterShortcuts` | `a{ss}` token to GTK accelerator | `b`; conflict is a D-Bus error |
| `UnregisterShortcuts` | — | — |
| `SetSuspended` | `b` | —; temporarily release/reacquire this sender's grabs |
| `SetClipboard` | UTF-8 `s` | nonzero `u` serial after owning the native selection |
| `Release` | — | release this sender's resources |

Signals: `ShortcutReleased(s token)` is unicast after both the actual main-key
release and all keyboard modifiers release; `ForegroundChanged(as)` reports
focus identity changes; `ClipboardConsumed(u serial)` is unicast when the
compositor requests a stream of the immutable text bytes.

Clipboard consumption confirms a data read, **not** that the intended target
inserted the text. A clipboard manager can also read the selection. Real target
paste behavior is verified separately by the native Qt receiver integration
test. The target must support standard Ctrl+V paste. Clipboard contents are
replaced; no clipboard-history service is added.

## Native compositor acceptance test

`tests/run-gnome-wayland.sh BUILD/logi-linux-wayland-tests` creates a private
session bus and temporary XDG directories, installs/enables the extension only
there, and starts `gnome-shell --nested --wayland --no-x11` on outer Xvfb.
The test clients use `QT_QPA_PLATFORM=wayland`; Xwayland is disabled inside the
compositor. Logs and actual client screenshots go to `LOGI_TEST_EVIDENCE`.

The production uinput sink emits real Linux kernel events. Because nested Mutter
does not directly consume host libinput devices, `evdev-to-mutter.py` forwards
only events read from devices named `Logi Options Open virtual input` into
Mutter's native RemoteDesktop input API. This is an explicitly test-only
transport, not a claim of physical MX Master 3S or full desktop-session testing.
It requires `/dev/uinput` write and the test event nodes' read permissions.
On an ephemeral CI VM, `LOGI_TEST_ALLOW_DEVICE_ACL=1` permits a narrowly scoped
`sudo setfacl` read grant for those exact test device nodes. No permission change
is made by the production extension.

The runner stops the input transport and compositor and removes its private
session directories on success or failure. It does not install into the invoking
user's real GNOME extension directory or change their desktop settings.

## Primary API references

- [GNOME extension ES modules and lifecycle](https://gjs.guide/extensions/development/creating.html)
- [GJS D-Bus exported objects](https://gjs.guide/guides/gio/dbus.html)
- [GNOME Shell 46 accelerator ownership](https://github.com/GNOME/gnome-shell/blob/46.0/js/ui/shellDBus.js)
- [GNOME Shell 46 modal capture and restoration](https://github.com/GNOME/gnome-shell/blob/46.0/js/ui/main.js)
- [Mutter 46 selection-source API](https://github.com/GNOME/mutter/blob/46.0/src/meta/meta-selection-source.h)
- [Mutter 46 native RemoteDesktop test transport](https://github.com/GNOME/mutter/blob/46.0/data/dbus-interfaces/org.gnome.Mutter.RemoteDesktop.xml)
