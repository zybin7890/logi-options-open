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
## 后续接口验收（2026-11-13）

最后一次后台进程重启后，用户再次确认侧键、主滚轮、拇指滚轮和拇指手势全部正常。现场复核官方 Options+ 进程为 0，Updater 服务已停止并禁用；自研后台保持同一 PID，映射与设置就绪。

修复 Python API/MCP 对新 Windows CLI 重载确认格式的误判。8 项针对性回归通过，包括已应用、未确认、失败后仍保留已保存文件，以及旧 Windows/Linux 返回格式。真实 HTTP PUT 与独立 MCP stdio 子进程均将原配置原样重载到运行中的后台，收到 `confirmed:true`、`status:applied`、正确的 PID 和文件 SHA-256；个人配置字节及后台 PID 未变。此操作未重启后台。
