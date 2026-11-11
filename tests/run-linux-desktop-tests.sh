#!/bin/sh
set -eu
build="${1:-build}"
export XDG_SESSION_TYPE=x11
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb
mkdir -p "$build/test-evidence"
export LOGI_TEST_EVIDENCE="$(cd "$build/test-evidence" && pwd)"
openbox >"$build/test-evidence/openbox.log" 2>&1 &
wm_pid=$!
trap 'kill "$wm_pid" 2>/dev/null || true' EXIT INT TERM
# Wait for the actual window manager before the first Qt exposure assertion.
attempt=0
while ! xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q 'window id # 0x'; do
    kill -0 "$wm_pid"
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo 'Openbox did not publish its EWMH supporting window' >&2
        exit 1
    fi
    sleep 0.05
done
xprop -root _NET_SUPPORTING_WM_CHECK >"$LOGI_TEST_EVIDENCE/window-manager-ready.txt"
ctest --test-dir "$build" -E linux-uinput-kernel --output-on-failure -V --output-log "$build/test-evidence/desktop-tests.log"
