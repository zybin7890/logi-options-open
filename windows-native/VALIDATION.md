# Windows publication validation — 2026-11-13

Local Windows build: CMake, Ninja, MinGW-w64 GCC 16.2.0, C++26, Release.

| Check | Result |
| --- | --- |
| Native GUI and CLI compile/link | Passed |
| 35 production gesture lifecycle tests | Passed, 35/35 |
| CLI validation of repository default mappings | Passed |
| CLI disclaimer text | Passed |
| Physical MX Master 3S actions and settings | Not tested for this candidate |
| Login startup, restart and DPI persistence | Not tested for this candidate |
| Visible UI, animation and interaction | Not tested for this public build |
| Linux | Unchanged, not tested in this upload |

The gesture tests include production backend code with fake HID, foreground,
clock, process operations and SendInput. They do not start the real daemon or
inject real input. No UI/backend deployment or automatic takeover was performed.

The publication adapts the existing Win32 candidate to repository-owned artwork
and the shared first-use notice. The notice include order and CLI JSON boolean
helper name were adjusted for Windows SDK declarations. No original artwork,
icon font, extracted binary, reverse-engineering dump, personal configuration,
device identifier or machine-specific path is part of this source snapshot.

The GUI and backend in this directory are a candidate, not a stable release.
Continuous media gestures remain unimplemented here. See README.md for further
functional gaps and reproduction commands.
