#!/bin/sh
set -eu
uuid='logi-options-open@zybin7890.github.io'
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
destination="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$uuid"
case "${1:---help}" in
  --install)
    mkdir -p "$destination"
    cp "$source_dir/extension.js" "$source_dir/metadata.json" "$destination/"
    printf '%s\n' "Installed optional third-party experimental extension: $destination" \
      'Enable it explicitly with install.sh --enable. If GNOME has not discovered it yet, log out and in first.'
    ;;
  --enable)
    gnome-extensions enable "$uuid"
    printf '%s\n' 'Enabled native desktop integration. Check logi-cli desktop-status for actual capabilities.'
    ;;
  --disable)
    gnome-extensions disable "$uuid"
    ;;
  --help)
    printf '%s\n' 'Optional GNOME Shell 46+ integration for Logi Options Open.' \
      'Third-party, unaffiliated with Logitech, experimental.' \
      'Enabling permits this bridge to read foreground app identity, reserve requested global shortcuts and replace clipboard text.' \
      'No extension is installed or enabled by the application automatically.' \
      'Usage: install.sh --install | --enable | --disable'
    ;;
  *) printf '%s\n' 'Unknown option; use --help' >&2; exit 2 ;;
esac
