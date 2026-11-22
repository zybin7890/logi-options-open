#!/bin/sh
set -eu
build="${1:-build}"
export XDG_SESSION_TYPE=x11
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb
mkdir -p "$build/test-evidence"
export LOGI_TEST_EVIDENCE="$(cd "$build/test-evidence" && pwd)"
export LC_MESSAGES=C
for tool in xprop xmessage xwininfo; do command -v "$tool"; done
openbox >"$build/test-evidence/openbox.log" 2>&1 &
wm_pid=$!
probe_pid=''
cleanup() {
    if [ -n "$probe_pid" ]; then
        kill "$probe_pid" 2>/dev/null || true
        wait "$probe_pid" 2>/dev/null || true
    fi
    kill "$wm_pid" 2>/dev/null || true
    wait "$wm_pid" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 1' INT TERM
attempt=0
while ! xprop -root _NET_SUPPORTING_WM_CHECK >"$LOGI_TEST_EVIDENCE/window-manager-ready.txt" 2>&1 || ! grep -q 'window id # 0x' "$LOGI_TEST_EVIDENCE/window-manager-ready.txt"; do
    kill -0 "$wm_pid"
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo 'Openbox did not publish its EWMH supporting window' >&2
        cat "$LOGI_TEST_EVIDENCE/window-manager-ready.txt"
        exit 1
    fi
    sleep 0.05
done

# _NET_SUPPORTING_WM_CHECK is published during startup. It does not prove
# Openbox has served a MapRequest and exposed a client inside its frame.
probe_title="Logi X11 readiness $$"
xmessage -title "$probe_title" -geometry 200x60+16+16 'X11 readiness' >"$LOGI_TEST_EVIDENCE/x11-probe.log" 2>&1 &
probe_pid=$!
attempt=0
while :; do
    kill -0 "$wm_pid" && kill -0 "$probe_pid"
    if xwininfo -name "$probe_title" -stats -tree >"$LOGI_TEST_EVIDENCE/x11-probe-window.txt" 2>&1 &&
       grep -q 'Map State: IsViewable' "$LOGI_TEST_EVIDENCE/x11-probe-window.txt"; then
        probe_id=$(awk '/^xwininfo: Window id:/ {print $4}' "$LOGI_TEST_EVIDENCE/x11-probe-window.txt")
        root_id=$(awk '/Root window id:/ {print $4}' "$LOGI_TEST_EVIDENCE/x11-probe-window.txt")
        parent_id=$(awk '/Parent window id:/ {print $4}' "$LOGI_TEST_EVIDENCE/x11-probe-window.txt")
        if [ -n "$probe_id" ] && [ -n "$root_id" ] && [ -n "$parent_id" ] && [ "$root_id" != "$parent_id" ] &&
           xprop -id "$probe_id" WM_STATE >"$LOGI_TEST_EVIDENCE/x11-probe-state.txt" 2>&1 &&
           grep -q 'window state: Normal' "$LOGI_TEST_EVIDENCE/x11-probe-state.txt" &&
           xprop -root _NET_CLIENT_LIST >"$LOGI_TEST_EVIDENCE/x11-probe-clients.txt" &&
           grep -qwF "$probe_id" "$LOGI_TEST_EVIDENCE/x11-probe-clients.txt"; then
            break
        fi
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo 'Openbox did not reparent and map the readiness client' >&2
        cat "$LOGI_TEST_EVIDENCE/x11-probe-window.txt" >&2
        exit 1
    fi
    sleep 0.05
done
kill "$probe_pid"
wait "$probe_pid" 2>/dev/null || true
probe_pid=''
attempt=0
while xprop -root _NET_CLIENT_LIST | grep -qwF "$probe_id"; do
    kill -0 "$wm_pid"
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then echo 'Openbox did not release the readiness client' >&2; exit 1; fi
    sleep 0.05
done
ctest --test-dir "$build" -E linux-uinput-kernel --output-on-failure -V --output-log "$build/test-evidence/desktop-tests.log"
