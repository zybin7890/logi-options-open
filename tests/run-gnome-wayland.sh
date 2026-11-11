#!/usr/bin/env bash
# Isolated actual GNOME Wayland session. The outer Xvfb is only its display;
# --no-x11 forbids Xwayland inside the compositor under test.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(cd -- "$script_dir/../linux/gnome" && pwd)
if [[ "${1:-}" != --inside ]]; then
    exec xvfb-run -a -s '-screen 0 1920x1080x24' dbus-run-session bash "$0" --inside "$@"
fi
shift
if [[ $# == 0 ]]; then echo 'Usage: run-gnome-wayland.sh TEST_EXECUTABLE [arguments]' >&2; exit 2; fi
executable=$(realpath -- "$1")
shift
scratch=$(mktemp -d "${TMPDIR:-/tmp}/logi-gnome-test.XXXXXXXX")
evidence=${LOGI_TEST_EVIDENCE:-"$scratch/evidence"}
mkdir -p "$evidence"
evidence=$(realpath -- "$evidence")
shell_pid=''
transport_pid=''
cleanup() {
    status=$?
    trap - EXIT
    [[ -z "$transport_pid" ]] || kill "$transport_pid" 2>/dev/null || true
    [[ -z "$transport_pid" ]] || wait "$transport_pid" 2>/dev/null || true
    [[ -z "$shell_pid" ]] || kill "$shell_pid" 2>/dev/null || true
    [[ -z "$shell_pid" ]] || wait "$shell_pid" 2>/dev/null || true
    if mountpoint -q "$scratch/runtime/doc"; then fusermount3 -u "$scratch/runtime/doc" || true; fi
    # Only this invocation's freshly created private directory is removed.
    [[ "$scratch" == "${TMPDIR:-/tmp}/logi-gnome-test."* ]] && rm -rf -- "$scratch"
    exit "$status"
}
trap cleanup EXIT
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" XDG_CACHE_HOME="$scratch/cache"
export XDG_RUNTIME_DIR="$scratch/runtime"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export GSETTINGS_BACKEND=keyfile XDG_CURRENT_DESKTOP=GNOME XDG_SESSION_TYPE=wayland
export GNOME_SHELL_SESSION_MODE=user LIBGL_ALWAYS_SOFTWARE=1
uuid='logi-options-open@zybin7890.github.io'
extension_dir="$XDG_DATA_HOME/gnome-shell/extensions/$uuid"
mkdir -p "$extension_dir" "$XDG_DATA_HOME/applications"
cp "$source_dir/extension.js" "$source_dir/metadata.json" "$extension_dir/"
native_dir="$(dirname -- "$executable")/linux/gnome/native"
mkdir -p "$extension_dir/native"
cp "$native_dir/liblogi-clipboard.so" "$native_dir/LogiClipboard-1.0.typelib" "$extension_dir/native/"
for id in Editor Browser; do
    cat > "$XDG_DATA_HOME/applications/org.logi.test.$id.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Logi native Wayland test $id
Exec="$executable" --receiver org.logi.test.$id /tmp/logi-unused-$id.json
StartupWMClass=org.logi.test.$id
Terminal=false
EOF
done
gsettings set org.gnome.shell enabled-extensions "['$uuid']"
gsettings set org.gnome.shell disable-user-extensions false
if gsettings list-keys org.gnome.shell | grep -qx welcome-dialog-last-shown-version; then
    gsettings set org.gnome.shell welcome-dialog-last-shown-version '999'
fi
dbus-update-activation-environment XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME XDG_RUNTIME_DIR \
    GSETTINGS_BACKEND XDG_CURRENT_DESKTOP XDG_SESSION_TYPE GNOME_SHELL_SESSION_MODE DISPLAY
gnome-shell --nested --wayland --no-x11 --wayland-display=logi-test > "$evidence/gnome-shell.log" 2>&1 &
shell_pid=$!
export WAYLAND_DISPLAY=logi-test QT_QPA_PLATFORM=wayland LOGI_TEST_EVIDENCE="$evidence"
export LOGI_WAYLAND_INPUT_READY="$scratch/input-ready"
for attempt in $(seq 1 160); do
    if gdbus call --session --dest org.logi.OptionsOpen.Desktop1 --object-path /org/logi/OptionsOpen/Desktop1 \
        --method org.logi.OptionsOpen.Desktop1.GetCapabilities > "$evidence/gnome-capabilities.txt" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$shell_pid" 2>/dev/null; then cat "$evidence/gnome-shell.log" >&2; exit 1; fi
    if [[ "$attempt" == 160 ]]; then cat "$evidence/gnome-shell.log" >&2; echo 'Native GNOME extension did not become available' >&2; exit 1; fi
    sleep 0.25
done
# A fresh GNOME profile starts in Overview, which intentionally keeps keyboard
# focus in Shell. Wait for actual startup completion, then leave Overview through
# its native D-Bus property before testing normal application focus behavior.
for attempt in $(seq 1 160); do
    if grep -q 'GNOME Shell started at' "$evidence/gnome-shell.log"; then break; fi
    if ! kill -0 "$shell_pid" 2>/dev/null; then cat "$evidence/gnome-shell.log" >&2; exit 1; fi
    if [[ "$attempt" == 160 ]]; then echo 'GNOME did not finish startup' >&2; exit 1; fi
    sleep 0.25
done
gdbus call --session --dest org.gnome.Shell --object-path /org/gnome/Shell \
    --method org.freedesktop.DBus.Properties.Get org.gnome.Shell OverviewActive > "$evidence/gnome-overview-before.txt"
gdbus call --session --dest org.gnome.Shell --object-path /org/gnome/Shell \
    --method org.freedesktop.DBus.Properties.Set org.gnome.Shell OverviewActive '<false>' >/dev/null
for attempt in $(seq 1 40); do
    gdbus call --session --dest org.gnome.Shell --object-path /org/gnome/Shell \
        --method org.freedesktop.DBus.Properties.Get org.gnome.Shell OverviewActive > "$evidence/gnome-overview-ready.txt"
    if grep -q false "$evidence/gnome-overview-ready.txt"; then break; fi
    if [[ "$attempt" == 40 ]]; then echo 'GNOME Overview did not close' >&2; exit 1; fi
    sleep 0.1
done
python3 "$source_dir/evdev-to-mutter.py" > "$evidence/gnome-input-transport.log" 2>&1 &
transport_pid=$!
"$executable" "$@" > >(tee "$evidence/gnome-wayland-test.log") 2>&1
kill -0 "$transport_pid" # A crashed transport must not count as success.
gui_test="$(dirname -- "$executable")/logi-linux-gui-tests"
if [[ -x "$gui_test" ]]; then
    LOGI_TEST_REQUIRE_NATIVE_WAYLAND=1 "$gui_test" > >(tee "$evidence/gnome-gui-test.log") 2>&1
else
    echo "Required native GUI interaction executable not found: $gui_test" >&2
    exit 1
fi
