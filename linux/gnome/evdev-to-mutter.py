#!/usr/bin/env python3
"""CI-only transport: real kernel evdev -> a nested Mutter Wayland compositor.

Only devices created by this project's production uinput sink are opened.
No key event is generated without a corresponding observed kernel event.
Mutter's private test transport is not a production desktop integration API.
"""
import errno
import glob
import os
import signal
import struct
import subprocess
from pathlib import Path

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

NAME = 'org.gnome.Mutter.RemoteDesktop'
INTERFACE = NAME + '.Session'
EVENT = struct.Struct('llHHi')
EXPECTED_NAME = 'Logi Options Open virtual input'
connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
session = connection.call_sync(NAME, '/org/gnome/Mutter/RemoteDesktop', NAME, 'CreateSession',
                               None, GLib.VariantType.new('(o)'), Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]


def call(method, signature=None, values=None):
    return connection.call_sync(NAME, session, INTERFACE, method,
                                GLib.Variant(signature, values) if signature else None,
                                None, Gio.DBusCallFlags.NONE, 5000, None)


call('Start')
devices = {}
held = {}
event_count = 0
loop = GLib.MainLoop()
ready = os.environ.get('LOGI_WAYLAND_INPUT_READY', '')


def mark_ready():
    if ready:
        Path(ready).write_text('\n'.join(item['sysname'] for item in devices.values()) + '\n')


def key(code, value, device):
    if value == 2:  # Mutter generates repeats from the observed down/up states.
        return
    held_by = held.setdefault(code, set())
    before = bool(held_by)
    if value:
        held_by.add(device)
    else:
        held_by.discard(device)
    after = bool(held_by)
    if before == after:
        return
    if code >= 0x100:
        call('NotifyPointerButton', '(ib)', (code, after))
    else:
        call('NotifyKeyboardKeycode', '(ub)', (code, after))


def remove(path):
    device = devices.pop(path, None)
    if not device:
        return
    for code, owners in list(held.items()):
        if path in owners:
            key(code, 0, path)
    os.close(device['fd'])
    mark_ready()


def input_ready(fd, condition, path):
    global event_count
    try:
        while True:
            packet = os.read(fd, EVENT.size * 64)
            if not packet:
                remove(path)
                return GLib.SOURCE_REMOVE
            if len(packet) % EVENT.size:
                raise RuntimeError('Partial kernel input_event frame')
            for _seconds, _microseconds, kind, code, value in EVENT.iter_unpack(packet):
                event_count += 1
                if kind == 1:
                    key(code, value, path)
                elif kind == 2 and code in (0, 1):
                    call('NotifyPointerMotionRelative', '(dd)', (float(value) if code == 0 else 0.0,
                                                               float(value) if code == 1 else 0.0))
                elif kind == 2 and code in (6, 8):
                    call('NotifyPointerAxisDiscrete', '(ui)', (1 if code == 6 else 0, -value))
    except BlockingIOError:
        return GLib.SOURCE_CONTINUE
    except OSError as error:
        if error.errno in (errno.ENODEV, errno.EIO):
            remove(path)
            return GLib.SOURCE_REMOVE
        raise


def scan():
    for entry in glob.glob('/sys/class/input/event*/device/name'):
        try:
            if Path(entry).read_text().strip() != EXPECTED_NAME:
                continue
            node = Path(entry).parents[1].name
            path = '/dev/input/' + node
            if path in devices:
                continue
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            except PermissionError:
                if os.environ.get('LOGI_TEST_ALLOW_DEVICE_ACL') != '1':
                    raise
                if (os.environ.get('GITHUB_ACTIONS') != 'true' or
                        os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted' or
                        os.environ.get('RUNNER_OS') != 'Linux'):
                    raise RuntimeError('Device ACL changes require a disposable GitHub-hosted Linux runner')
                # Only the exact-named temporary uinput device in this CI VM.
                subprocess.run(['sudo', '-n', 'setfacl', '-m', f'u:{os.getuid()}:r', path], check=True)
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            sysname = Path(entry).parent.resolve().name
            devices[path] = {'fd': fd, 'sysname': sysname}
            GLib.io_add_watch(fd, GLib.PRIORITY_DEFAULT, GLib.IOCondition.IN | GLib.IOCondition.HUP | GLib.IOCondition.ERR,
                             input_ready, path)
            mark_ready()
            print(f'Forwarding real kernel {sysname} / {path} to native Mutter', flush=True)
        except FileNotFoundError:
            continue  # Device disappeared during hotplug enumeration.
    return GLib.SOURCE_CONTINUE


def stop(_signal, _frame):
    loop.quit()


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
scan()
GLib.timeout_add(50, scan)
print('Native Mutter transport ready; waiting for project uinput device', flush=True)
try:
    loop.run()
finally:
    for path in list(devices):
        remove(path)
    call('Stop')
    print(f'Forwarded {event_count} observed kernel events', flush=True)
