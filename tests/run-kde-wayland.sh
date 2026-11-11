#!/usr/bin/env bash
set -euo pipefail
driver="$(realpath "${1:?usage: run-kde-wayland.sh path/to/logi-linux-wayland-tests}")"
if [[ "${LOGI_KDE_TEST_SESSION:-}" != 1 ]]; then
    exec dbus-run-session -- env LOGI_KDE_TEST_SESSION=1 bash "$0" "$driver"
fi
bindir="$(dirname "$driver")"
evidence="${LOGI_TEST_EVIDENCE:-$bindir/test-evidence}"
mkdir -p "$evidence"
evidence="$(realpath "$evidence")"
session="$(mktemp -d)"
export XDG_RUNTIME_DIR="$session/runtime" XDG_CONFIG_HOME="$session/config" XDG_DATA_HOME="$session/data" XDG_CACHE_HOME="$session/cache"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
chmod 700 "$XDG_RUNTIME_DIR"
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=KDE KDE_FULL_SESSION=true QT_QPA_PLATFORM=wayland
export WAYLAND_DISPLAY=loo-kde-wayland LIBGL_ALWAYS_SOFTWARE=1 KWIN_COMPOSE=Q
export LOGI_WAYLAND_INPUT_READY="$session/input-ready"
unset DISPLAY
# Session services activated by KWin must connect to this isolated compositor,
# not inherit the outer CI display captured when dbus-run-session started.
dbus-update-activation-environment XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME XDG_SESSION_TYPE XDG_CURRENT_DESKTOP KDE_FULL_SESSION QT_QPA_PLATFORM WAYLAND_DISPLAY
pids=()
provider_pid=""
cleanup() {
    if [[ -n "$provider_pid" ]]; then ps -p "$provider_pid" -o pid,comm,rss,pcpu >"$evidence/kde-provider-resource-snapshot.txt" || true; fi
    for ((i=${#pids[@]}-1; i>=0; --i)); do kill "${pids[$i]}" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    # No host configuration is touched; retain the isolated config with evidence.
    cp -a "$XDG_CONFIG_HOME" "$evidence/kde-session-config" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# KWin authorizes this test-only protocol using executable identity from the
# desktop database. Keep its grant inside the disposable session data directory.
mkdir -p "$XDG_DATA_HOME/applications"
cat >"$XDG_DATA_HOME/applications/org.logi.OptionsOpen.InputTest.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Logi Options Open isolated input test
Exec=$bindir/logi-kde-input-forwarder
NoDisplay=true
X-KDE-Wayland-Interfaces=org_kde_kwin_fake_input
EOF
kbuildsycoca5 --noincremental >"$evidence/kde-desktop-database.log" 2>&1

# Virtual KWin has no physical/logind seat. It is a real Wayland compositor;
# the test-only forwarder transports actual uinput/evdev events to fake-input.
QT_LOGGING_RULES='kwin_core.debug=true;KWIN_UTILS.debug=true' kwin_wayland --virtual --no-lockscreen --socket "$WAYLAND_DISPLAY" --width 1280 --height 900 >"$evidence/kde-kwin.log" 2>&1 &
pids+=("$!")
for ((i=0; i<200; ++i)); do [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break; kill -0 "${pids[0]}"; sleep 0.05; done
[[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] || { cat "$evidence/kde-kwin.log"; exit 1; }

# Exercise the default Plasma clipboard-manager path as well as bare KWin.
klipper >"$evidence/kde-klipper.log" 2>&1 &
pids+=("$!")
for ((i=0; i<200; ++i)); do
    if dbus-send --session --type=method_call --print-reply --dest=org.kde.klipper /klipper org.freedesktop.DBus.Introspectable.Introspect >"$evidence/kde-klipper-interface.log" 2>&1; then break; fi
    kill -0 "${pids[-1]}"; sleep 0.05
done
dbus-send --session --type=method_call --print-reply --dest=org.kde.klipper /klipper org.freedesktop.DBus.Introspectable.Introspect >"$evidence/kde-klipper-interface.log"

# Only in an explicitly opted-in disposable CI VM, allow keyboard-state queries
# and access to the exact virtual input devices created by this test.
if [[ "${LOGI_TEST_ALLOW_DEVICE_ACL:-}" == 1 ]]; then
    [[ "${GITHUB_ACTIONS:-}" == true && "${RUNNER_ENVIRONMENT:-}" == github-hosted && "${RUNNER_OS:-}" == Linux ]] || {
        echo 'Device ACL changes are restricted to disposable GitHub-hosted Linux runners.' >&2; exit 1;
    }
    # GitHub's disposable VM includes an emulated keyboard. Complete modifier
    # release checks require its bitmap too; this opt-in is CI-only.
    for name in /sys/class/input/event*/device/name; do
        [[ -f "$name" ]] || continue
        device_name="$(cat "$name")"
        [[ "${device_name,,}" == *keyboard* ]] || continue
        event="${name%/device/name}"; event="${event##*/}"
        sudo -n setfacl -m "u:$(id -u):r" "/dev/input/$event"
    done
    (
        while kill -0 "${pids[0]}" 2>/dev/null; do
            for name in /sys/class/input/event*/device/name; do
                [[ -f "$name" ]] || continue
                [[ "$(cat "$name")" == "Logi Options Open virtual input" ]] || continue
                event="${name%/device/name}"; event="${event##*/}"
                sudo -n setfacl -m "u:$(id -u):r" "/dev/input/$event"
            done
            sleep 0.05
        done
    ) >"$evidence/kde-input-acl.log" 2>&1 &
    pids+=("$!")
fi
"$bindir/logi-kde-input-forwarder" >"$evidence/kde-input-transport.log" 2>&1 &
pids+=("$!")
LOGI_KDE_TRACE=1 "$bindir/logi-kde-provider" >"$evidence/kde-provider.log" 2>&1 &
provider_pid="$!"; pids+=("$provider_pid")
ready=0
for ((i=0; i<200; ++i)); do
    if dbus-send --session --type=method_call --print-reply --dest=org.logi.OptionsOpen.Desktop1 /org/logi/OptionsOpen/Desktop1 org.logi.OptionsOpen.Desktop1.GetCapabilities >"$evidence/kde-capabilities.log" 2>&1; then
        # Foreground becomes available only after the loaded KWin script reports
        # its first native snapshot. Keyboard capability can be false here: the
        # driver creates the real uinput keyboard after this provider handshake.
        if grep -A1 'string "foreground"' "$evidence/kde-capabilities.log" | grep -q 'boolean true' &&
           grep -A1 'string "clipboard"' "$evidence/kde-capabilities.log" | grep -q 'boolean true'; then ready=1; break; fi
    fi
    kill -0 "${pids[-1]}"; sleep 0.05
done
[[ "$ready" == 1 ]] || { cat "$evidence/kde-capabilities.log" "$evidence/kde-provider.log"; exit 1; }
"$driver" 2>&1 | tee "$evidence/kde-wayland-test.log"
LOGI_TEST_REQUIRE_NATIVE_WAYLAND=1 "$bindir/logi-linux-gui-tests" 2>&1 | tee "$evidence/kde-gui-test.log"
LOGI_TEST_REQUIRE_NATIVE_WAYLAND=1 "$bindir/logi-linux-gui-tests" --standalone-ring 2>&1 | tee "$evidence/kde-standalone-ring-test.log"
