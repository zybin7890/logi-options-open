# Windows publication validation — 2026-11-13

Local Windows build: CMake, Ninja, MinGW-w64 GCC 16.2.0, C++26, Release.

| Check | Result |
| --- | --- |
| Native GUI and CLI compile/link | Passed |
| Production gesture lifecycle and continuous media tests | Passed, 66/66 (35 legacy + 31 media) |
| CLI validation of repository default mappings | Passed |
| CLI disclaimer text | Passed |
| Physical MX Master 3S side buttons, both wheels and existing gestures | User confirmed working with official processes absent |
| Continuous media gesture physical behavior | User confirmed volume, tracks, tap and release behavior |
| DPI and five wheel setting owner round trips | Passed; readback, persistence and restore confirmed |
| Daemon restart and saved DPI reload | Passed via login launcher; hardware 5050 restored to saved 5000 |
| Actual Windows logon/reboot | Not tested |
| Preset selection and direction edit in actual legacy Win32 window | Passed with isolated config and device/process boundaries |
| Deployed legacy GUI DPI display | Device label and edit field both loaded 5000; closing GUI preserved daemon |
| Full UI, animation and interaction | Not accepted for this public build |
| Linux | Unchanged, not tested in this upload |

The gesture tests include production backend code with fake HID, foreground,
clock, process operations and SendInput. They do not start the real daemon or
inject real input. Separate authorized live checks and local deployment followed;
see LIVE-VALIDATION.md. Offline results alone are not hardware acceptance.

The publication adapts the existing Win32 candidate to repository-owned artwork
and the shared first-use notice. The notice include order and CLI JSON boolean
helper name were adjusted for Windows SDK declarations. No original artwork,
icon font, extracted binary, reverse-engineering dump, personal configuration,
device identifier or machine-specific path is part of this source snapshot.

Six existing GUI/CLI DPI, reload and settings entry harnesses also passed against
the updated working source. The visible preview verified continuous preset selection,
immediate save and changing a direction back to ordinary custom mode. Its device
and IPC boundaries were mocked: the UI's applied message is not a hardware result.

The GUI and backend in this directory are a candidate, not a stable release.
Continuous media gestures are implemented with explicit local threshold and release
policies; the tested device worked, but complete original semantic equivalence and
long-term stability are unverified. See README.md and
REPLACEMENT-ACCEPTANCE.md for gaps and reproduction commands.
