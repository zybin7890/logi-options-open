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
# Desktop tests wait for their own mapped/active windows; no fixed startup sleep.
ctest --test-dir "$build" -E linux-uinput-kernel --output-on-failure -V
