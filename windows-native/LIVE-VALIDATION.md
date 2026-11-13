# MX Master 3S Windows live checks — 2026-11-13

These results concern the local Win32 candidate/backend represented by this source.
The local GUI uses locally available legacy artwork. Public builds use repository
schematic artwork and the shared first-use notice; exact visual equivalence of the
public resource build is not claimed. No private profiles, extracted assets or
device serials are published with this report.

## Exclusive operation

The initial takeover attempt was not accepted: the official Updater service
restarted Options+ Agent. The candidate detected the conflict and refused DPI
writes. The user's first positive input report during this overlap was excluded.

At the user's explicit request, official Agent/Updater/AppBroker processes were
stopped, the official updater service disabled and official Agent/AppBroker Run
entries removed. Restoration records were saved locally. This is a machine setup
action, not an automatic service-disabling behavior added to the application.

The candidate then ran with official processes absent. The user confirmed side
buttons, main wheel, thumb wheel, task-view tap and desktop gestures working.
The backend reported ready mappings/settings, no device/input failures and no
reporting repairs during the accepted checks.

## Settings and continuous media

- DPI: 5000 → 5050 → 5000. Both owner requests confirmed hardware readback and
  persistence; elapsed CLI request times were 384.64 ms and 106.95 ms (including
  process/tool overhead, not raw HID latency).
- SmartShift: 12 → 13 → 12; ratchet: 1 → 0 → 1; high-resolution: 0 → 1 → 0;
  main-wheel inversion: 0 → 1 → 0; thumb inversion: 0 → 1 → 0. Every request
  confirmed apply/save/readback. The diverted thumb direction was explicitly
  reported as software mapping, not falsely represented as a hardware readback.
  Request times ranged from 105.33 ms to 998.66 ms; the first SmartShift change
  was the slowest. No sub-100 ms latency guarantee is made.
- Continuous media preset was temporarily enabled through confirmed hot reload
  without restarting the daemon. The user confirmed held-motion volume changes,
  no stationary repeat, no extra play/pause on release, track changes and tap
  play/pause. Original virtual-desktop bindings were then restored with confirmed
  reload and matching profile fingerprint.

## Startup and GUI

The tested local binaries were copied to the existing desktop shortcut's target,
with the previous binaries/configuration retained as a rollback copy. A user-level
Run entry invokes a hidden launcher for the standalone daemon, independent of the
GUI. Official startup remains disabled.

To test actual DPI loading, the daemon was stopped, hardware was set to 5050 using
a separate test directory (leaving the installed 5000 sidecar untouched), and the
login launcher started a new daemon. Hardware readback was 5000 and the log
reported restoring the saved pointer speed. This proves process-start restoration;
it does **not** prove a Windows reboot or actual logon session.

The installed legacy GUI was opened afterward. Its device label and pointer-speed
input both showed 5000. Closing the GUI left the same healthy daemon running.
No physical-input retest after this final daemon restart has been recorded yet.

## Short resource sample

With the GUI closed: 30 samples over 29.54 seconds; working set 8.504–8.520 MiB,
private bytes peak 1.445 MiB, handles 146 throughout, accumulated process CPU
increase 0.046875 seconds (about 0.159% of one core over the interval). No official
process appeared during these samples. This is a short observation, not a soak
test or proof of no memory leak.

Actual reboot/logon, suspend/resume, unplug/reconnect cycles, long-term operation,
full UI/motion review, remaining gesture semantics, Flow and other missing
features remain open in REPLACEMENT-ACCEPTANCE.md. No stable Release is made.
