# Isolated Windows runtime tests

Configure `windows-native` with MinGW-w64, CMake 4.1 or newer, and `BUILD_TESTING=ON`.
Build the test targets, then run `ctest --test-dir <build-directory> --output-on-failure`.
The tests compile the production `remap.cpp`; they do not duplicate its parser,
sequence scheduler, or device event loop.

| Suite | Production behavior exercised |
| --- | --- |
| `recorded_keys-production` | `KeyDown`/`KeyUp` parsing, left/right modifiers and OEM VK values, the 1024-event limit, holds across waits, trailing waits, shared ownership, physical key takeover, foreground cancellation, rejected/partial injection, and bounded cleanup |
| `recorded-events-*` | Actual `run_agent`/`events` entry points: normal completion, stop, disconnect/reconnect, input notification loss, reload, and foreground change while keys are held |
| `preview-events-*` | Actual `play_actions` entry point: validation executes nothing, the three-second countdown, held-key execution/cleanup, refused application activation, rejected input, focus cancellation, and legacy failed-Open continuation |
| `ring_slots-production` | Eight-slot positioning, true empty slots, empty folders, legacy `None`, nested folders and Back navigation, shared subfolders, missing targets and cyclic configuration rejection |
| `app_actions-production` | Exact executable-path matching, foreground readback, target restoration/minimize/maximize/normal close, refused activation, unavailable targets, and focus ownership during delayed shortcuts |
| Existing `gesture-*` and `media-*` | Gesture lifecycle, application ownership, concurrent sequences, continuous media actions, and input failure cleanup |

All input injection and keyboard-hook installation in these tests are replaced by
capture functions. `reject_effects.hpp` rejects unconfigured input, hook, launch,
window, or workstation-lock effects. The physical-key observer test uses its own
thread message queue with a fake hook callback; it never registers a Windows hook.
The runtime event fixtures provide synthetic HID replies and never connect to a
mouse. Assertions in the standalone suites remain enabled in Release builds.

These checks prove the exercised parser, scheduler, ownership, and event-loop
behavior under controlled failures. They do not establish real mouse firmware,
Bluetooth/Bolt transport, Windows focus permissions, input delivery to real
applications, GUI appearance, or hardware acceptance.

`Open(...)` retains the established behavior of continuing after a failed launch.
`AppForeground(<UTF-8 absolute-path hex>)` has a stricter contract: if the selected
application cannot be brought to the foreground and read back as the same
window/PID, the sequence stops before subsequent shortcuts. `AppClose` requests
normal `WM_CLOSE` and preserves the application's save/confirmation behavior.
Preview uses the same `Sequences` executor as the agent. Its exit statuses are
`0` for completion, `2` for invalid/unsupported preview input, `3` for execution
failure, and `4` for focus cancellation. A failed foreground request never falls
through to the following shortcut.
