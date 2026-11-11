# Windows reporting recovery acceptance, 2026-11-11

The original Options+ installation and services were absent during these tests.
The native C++ backend does not require them. Restoration of a previously paused
Options+ process is optional; an uninstalled executable is now skipped.

An MX Master 3S lost custom input while the previous daemon process remained
alive. The old process-existence status and log did not establish the precise
historical trigger. Resetting standard hardware reporting restored normal input,
confirmed by the user. Code inspection found no runtime diversion readback and a
permanent exit on a transient activation failure during reconnection.

The correction checks device reporting after session/HID input activity, repairs
lost button/gesture/thumb diversion and an incorrect main-wheel HID++ target,
and retries activation failures. Idle sessions do not generate periodic radio
queries. Any session activity, including keyboard input, can trigger a check.
The actual idle-device main-wheel read took 516–547 ms; that read uses the existing
1200 ms device-request limit, rather than the initially tested, insufficient
300 ms limit. These measured successful replies were not counted as disconnects.

`agent-status` now distinguishes process existence from last verified readiness
and reports device notifications, readback age, reconnects, repairs, device
failures and Windows injection failures. A stale readback is visible through
`checked_ago_ms`; it is not a continuous assertion that a sleeping device is awake.

The explicit `logi-windows-reporting-recovery-test` executable resets reporting
for side control 0x0053, gesture control 0x00C3, thumb wheel and main wheel on an
actual MX Master 3S. It then simulates resumed pointer activity and requires all
four hardware states to read back correctly within seven seconds. It is not
registered with CTest or installed in packages. Run it only deliberately with an
active native daemon and `--reset-reporting-while-agent-runs`.

Observed results:

- Candidate PID 15020: connections 1, automatic repair 1, device failures 0,
  injection failures 0. The user confirmed side buttons, thumb wheel and gesture
  actions all recovered; the daemon recorded 44 actual HID++ events.
- Deployed PID 9932: a second real hardware fault injection passed, connections
  stayed 1, repair count became 1, device/injection failures stayed 0.
- Original Options+ was absent throughout; user mappings were preserved.
- The candidate snapshot used about 8.0 MiB working set and 1.4 MiB private
  memory. This is a snapshot, not a long-duration performance benchmark.
- Public Windows source rebuilt; consent/platform and first-run tests passed.

This validates the specified reporting-reset recovery and user-visible actions.
It does not prove every possible sleep, radio-loss, suspend/resume, or long-running
failure has been eliminated. The new state counters allow those cases to be
distinguished if they recur.
