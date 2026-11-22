#!/bin/sh
set -eu
if [ "${1:-}" != --private-session ]; then
    session_dir=$(mktemp -d /tmp/logi-openbox.XXXXXXXX)
    trap 'rm -rf -- "$session_dir"' EXIT
    export XDG_CONFIG_HOME="$session_dir/config" XDG_DATA_HOME="$session_dir/data" XDG_RUNTIME_DIR="$session_dir/runtime"
    export LOGI_OPENBOX_READY="$session_dir/started"
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
    unset SESSION_MANAGER WAYLAND_DISPLAY
    dbus-run-session -- sh "$0" --private-session "$@"
    exit $?
fi
shift
build="${1:-build}"
export XDG_SESSION_TYPE=x11
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb
mkdir -p "$build/test-evidence"
export LOGI_TEST_EVIDENCE="$(cd "$build/test-evidence" && pwd)"
export LC_MESSAGES=C
for tool in xprop xmessage xwininfo; do command -v "$tool"; done
# Openbox executes --startup after client_manage_all and OB_STATE_RUNNING.
# EWMH properties are published earlier, during screen annexation.
openbox --sm-disable --debug --startup "/usr/bin/touch '$LOGI_OPENBOX_READY'" >"$build/test-evidence/openbox.log" 2>&1 &
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
stage_log="$LOGI_TEST_EVIDENCE/x11-startup.log"
mark() { printf '%s %s\n' "$(date -Ins)" "$*" >>"$stage_log"; }
diagnose() {
    mark "FAILED: $*"
    {
        cat "$stage_log"
        ps -p "$wm_pid${probe_pid:+,$probe_pid}" -o pid,ppid,stat,pcpu,time,etime,wchan:32,comm || true
        for thread in /proc/"$wm_pid"/task/*; do
            [ ! -r "$thread/wchan" ] || printf '%s wchan=%s\n' "$thread" "$(cat "$thread/wchan")"
        done
        timeout 2 xprop -root _NET_SUPPORTING_WM_CHECK _NET_CLIENT_LIST _NET_ACTIVE_WINDOW _NET_CURRENT_DESKTOP || true
        timeout 2 xwininfo -root -tree || true
        if [ -s "$LOGI_TEST_EVIDENCE/x11-probe-window.txt" ]; then cat "$LOGI_TEST_EVIDENCE/x11-probe-window.txt"; fi
        tail -n 80 "$LOGI_TEST_EVIDENCE/openbox.log"
    } >"$LOGI_TEST_EVIDENCE/x11-startup-failure.txt" 2>&1
    cat "$LOGI_TEST_EVIDENCE/x11-startup-failure.txt" >&2
    exit 1
}
mark "launched Openbox pid=$wm_pid, waiting for its --startup callback"
attempt=0
while [ ! -f "$LOGI_OPENBOX_READY" ]; do
    kill -0 "$wm_pid" 2>/dev/null || diagnose 'Openbox exited during initialization'
    attempt=$((attempt + 1))
    [ "$attempt" -lt 400 ] || diagnose 'Openbox did not complete initialization'
    sleep 0.05
done
mark 'Openbox startup callback completed'
xprop -root _NET_SUPPORTING_WM_CHECK >"$LOGI_TEST_EVIDENCE/window-manager-ready.txt"
grep -q 'window id # 0x' "$LOGI_TEST_EVIDENCE/window-manager-ready.txt" || diagnose 'Openbox did not claim the root'

# _NET_SUPPORTING_WM_CHECK is published during startup. It does not prove
# Openbox has served a MapRequest and exposed a client inside its frame.
probe_title="Logi X11 readiness $$"
xmessage -title "$probe_title" -geometry 200x60+16+16 'X11 readiness' >"$LOGI_TEST_EVIDENCE/x11-probe.log" 2>&1 &
probe_pid=$!
mark "canary requested a window, pid=$probe_pid"
attempt=0
while :; do
    kill -0 "$wm_pid" 2>/dev/null && kill -0 "$probe_pid" 2>/dev/null || diagnose 'Openbox or canary exited before mapping'
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
    [ "$attempt" -lt 100 ] || diagnose 'Openbox did not reparent and map the readiness client'
    sleep 0.05
done
mark "canary client $probe_id is viewable, Normal, with frame $parent_id"
kill "$probe_pid"
wait "$probe_pid" 2>/dev/null || true
probe_pid=''
attempt=0
while xprop -root _NET_CLIENT_LIST | grep -qwF "$probe_id"; do
    kill -0 "$wm_pid" 2>/dev/null || diagnose 'Openbox exited while releasing the canary'
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || diagnose 'Openbox did not release the readiness client'
    sleep 0.05
done
mark 'canary removed; starting CTest'
ctest --test-dir "$build" -E linux-uinput-kernel --output-on-failure -V --output-log "$build/test-evidence/desktop-tests.log"
