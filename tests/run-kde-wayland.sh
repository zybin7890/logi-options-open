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
pids=()
cleanup() {
    for ((i=${#pids[@]}-1; i>=0; --i)); do kill "${pids[$i]}" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    # No host configuration is touched; retain the isolated config with evidence.
    cp -a "$XDG_CONFIG_HOME" "$evidence/kde-session-config" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Virtual KWin has no physical/logind seat. It is a real Wayland compositor;
# the test-only forwarder transports actual uinput/evdev events to fake-input.
kwin_wayland --virtual --no-lockscreen --socket "$WAYLAND_DISPLAY" --width 1280 --height 900 >"$evidence/kde-kwin.log" 2>&1 &
pids+=("$!")
for ((i=0; i<200; ++i)); do [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break; kill -0 "${pids[0]}"; sleep 0.05; done
[[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] || { cat "$evidence/kde-kwin.log"; exit 1; }

# Only in this explicitly opted-in CI session, give the runner access to the
# exact virtual input devices made by the test; never alter a physical keyboard.
if [[ "${LOGI_TEST_ALLOW_DEVICE_ACL:-}" == 1 ]]; then
    (
        while kill -0 "${pids[0]}" 2>/dev/null; do
            for name in /sys/class/input/event*/device/name; do
                [[ -f "$name" ]] || continue
                [[ "$(cat "$name")" == "Logi Options Open virtual input" ]] || continue
                event="${name%/device/name}"; event="${event##*/}"
                sudo -n chmod a+rw "/dev/input/$event"
            done
            sleep 0.05
        done
    ) >"$evidence/kde-input-acl.log" 2>&1 &
    pids+=("$!")
fi
"$bindir/logi-kde-input-forwarder" >"$evidence/kde-input-transport.log" 2>&1 &
pids+=("$!")
"$bindir/logi-kde-provider" >"$evidence/kde-provider.log" 2>&1 &
pids+=("$!")
for ((i=0; i<200; ++i)); do
    if dbus-send --session --type=method_call --print-reply --dest=org.logi.OptionsOpen.Desktop1 /org/logi/OptionsOpen/Desktop1 org.logi.OptionsOpen.Desktop1.GetCapabilities >"$evidence/kde-capabilities.log" 2>&1; then break; fi
    kill -0 "${pids[-1]}"; sleep 0.05
done
"$driver" 2>&1 | tee "$evidence/kde-wayland-test.log"
"$bindir/logi-linux-gui-tests" 2>&1 | tee "$evidence/kde-gui-test.log"
