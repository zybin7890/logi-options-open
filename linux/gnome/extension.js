// SPDX-License-Identifier: MIT
// Runs inside GNOME Shell, not an Xwayland client. Installation is opt-in.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const NAME = 'org.logi.OptionsOpen.Desktop1';
const PATH = '/org/logi/OptionsOpen/Desktop1';
const XML = `<node><interface name="${NAME}">
 <method name="GetCapabilities"><arg type="a{sv}" direction="out"/></method>
 <method name="GetForeground"><arg type="as" direction="out"/></method>
 <method name="RegisterShortcuts"><arg type="a{ss}" direction="in"/><arg type="b" direction="out"/></method>
 <method name="UnregisterShortcuts"/>
 <method name="SetSuspended"><arg type="b" direction="in"/></method>
 <method name="SetClipboard"><arg type="s" direction="in"/><arg type="u" direction="out"/></method>
 <method name="Release"/>
 <signal name="ShortcutReleased"><arg type="s"/></signal>
 <signal name="ForegroundChanged"><arg type="as"/></signal>
 <signal name="ClipboardConsumed"><arg type="u"/></signal>
</interface></node>`;

// Each read returns a separate stream backed by immutable bytes. A later text
// action cannot change a stream already handed to a native Wayland client.
const ClipboardSource = GObject.registerClass(
class ClipboardSource extends Meta.SelectionSource {
    _init(text, consumed) {
        super._init();
        this._bytes = new GLib.Bytes(new TextEncoder().encode(text));
        this._consumed = consumed;
    }

    vfunc_get_mimetypes() { return ['text/plain;charset=utf-8', 'text/plain', 'UTF8_STRING']; }

    vfunc_read_async(_mime, cancellable, callback) {
        const task = Gio.Task.new(this, cancellable, callback);
        task.return_boolean(true);
    }

    vfunc_read_finish(result) {
        result.propagate_boolean();
        const stream = Gio.MemoryInputStream.new_from_bytes(this._bytes);
        this._consumed?.();
        return stream;
    }
});

export default class OptionsOpenBridge extends Extension {
    enable() {
        this._clients = new Map();
        this._actions = new Map();
        this._serial = 0;
        this._pending = null;
        this._releaseIdle = 0;
        this._source = null;
        this._sourceOwner = null;
        this._enabled = true;
        this._dbus = Gio.DBusExportedObject.wrapJSObject(XML, this);
        this._dbus.export(Gio.DBus.session, PATH);
        this._busId = Gio.bus_own_name_on_connection(Gio.DBus.session, NAME, Gio.BusNameOwnerFlags.NONE, null, null);
        this._activation = global.display.connect('accelerator-activated', (_display, action) => this._activated(action));
        this._focus = global.display.connect('notify::focus-window', () => {
            this._dbus.emit_signal('ForegroundChanged', new GLib.Variant('(as)', [this.GetForeground()]));
        });
    }

    disable() {
        this._enabled = false;
        this._cancelPending();
        for (const sender of [...this._clients.keys()]) this._release(sender);
        if (this._source) global.display.get_selection().unset_owner(Meta.SelectionType.CLIPBOARD, this._source);
        this._source = null;
        if (this._activation) global.display.disconnect(this._activation);
        if (this._focus) global.display.disconnect(this._focus);
        this._activation = this._focus = 0;
        if (this._busId) Gio.bus_unown_name(this._busId);
        this._busId = 0;
        this._dbus?.unexport();
        this._dbus = null;
    }

    GetCapabilities() {
        const native = Meta.is_wayland_compositor();
        return {
            protocol: new GLib.Variant('u', 1),
            desktop: new GLib.Variant('s', 'GNOME Shell'),
            native_wayland: new GLib.Variant('b', native),
            foreground: new GLib.Variant('b', native),
            shortcuts: new GLib.Variant('b', native),
            clipboard: new GLib.Variant('b', native),
            clipboard_ack: new GLib.Variant('b', native),
        };
    }

    GetForeground() {
        const window = global.display.focus_window;
        if (!window || !Meta.is_wayland_compositor()) return [];
        const values = [];
        // On native windows WM_CLASS is the xdg_toplevel app_id. GTK application
        // id and Shell's resolved .desktop id are additional exact-match names.
        for (const value of [window.get_gtk_application_id(), window.get_wm_class(),
            window.get_wm_class_instance(), Shell.WindowTracker.get_default().get_window_app(window)?.get_id()]) {
            if (!value) continue;
            const name = value.replace(/\.desktop$/i, '').toLowerCase();
            if (!values.includes(name)) values.push(name);
        }
        return values;
    }

    _client(sender) {
        if (!Meta.is_wayland_compositor()) throw new Error('Native GNOME Wayland session required');
        if (!this._clients.has(sender)) {
            const client = {bindings: new Map(), suspended: false, watch: 0};
            this._clients.set(sender, client);
            client.watch = Gio.bus_watch_name_on_connection(Gio.DBus.session, sender, Gio.BusNameWatcherFlags.NONE,
                null, () => this._release(sender));
        }
        return this._clients.get(sender);
    }

    _reply(invocation, action, type = null) {
        try {
            const result = action(invocation.get_sender());
            invocation.return_value(type ? new GLib.Variant(type, [result]) : null);
        } catch (error) {
            invocation.return_dbus_error(`${NAME}.Error`, error.message);
        }
    }

    RegisterShortcutsAsync([shortcuts], invocation) {
        this._reply(invocation, sender => {
            const client = this._client(sender);
            const requested = new Map(Object.entries(shortcuts));
            if (requested.size > 64) throw new Error('At most 64 shortcuts can be registered');
            const accelerators = new Set();
            for (const [token, accelerator] of requested) {
                if (!token || !accelerator || accelerators.has(accelerator)) throw new Error('Empty or duplicate shortcut');
                accelerators.add(accelerator);
            }
            // Keep old grabs until all new grabs succeed. Mutter rejects grabs
            // already registered by the user, Shell, or another client.
            const next = new Map();
            const added = [];
            try {
                for (const [token, accelerator] of requested) {
                    const existing = [...client.bindings.values()].find(binding => binding.action && binding.accelerator === accelerator);
                    const binding = existing ? {...existing, token} : this._grab(sender, token, accelerator);
                    if (!existing) added.push(binding);
                    next.set(token, binding);
                }
            } catch (error) {
                for (const binding of added) this._ungrab(binding);
                throw error;
            }
            const keep = new Set([...next.values()].map(binding => binding.action));
            for (const binding of client.bindings.values()) if (!keep.has(binding.action)) this._ungrab(binding);
            // Reload can occur during output injection. Probe the replacement
            // transaction, then retain its definitions without passive grabs.
            if (client.suspended) for (const binding of next.values()) this._ungrab(binding);
            client.bindings = next;
            for (const binding of next.values()) if (binding.action) this._actions.set(binding.action, binding);
            return true;
        }, '(b)');
    }

    UnregisterShortcutsAsync(_params, invocation) {
        this._reply(invocation, sender => {
            const client = this._clients.get(sender);
            if (!client) return;
            if (this._pending?.sender === sender) this._cancelPending();
            for (const binding of client.bindings.values()) this._ungrab(binding);
            client.bindings.clear();
        });
    }

    SetSuspendedAsync([suspended], invocation) {
        this._reply(invocation, sender => {
            const client = this._client(sender);
            if (suspended === client.suspended) return;
            if (this._pending?.sender === sender) this._cancelPending();
            if (suspended) {
                for (const binding of client.bindings.values()) this._ungrab(binding);
                client.suspended = true;
                return;
            }
            const next = new Map();
            try {
                for (const [token, binding] of client.bindings)
                    next.set(token, this._grab(sender, token, binding.accelerator));
            } catch (error) {
                for (const binding of next.values()) this._ungrab(binding);
                throw error;
            }
            client.bindings = next;
            client.suspended = false;
        });
    }

    SetClipboardAsync([text], invocation) {
        this._reply(invocation, sender => {
            this._client(sender);
            const serial = ++this._serial;
            let acknowledged = false;
            const source = new ClipboardSource(text, () => {
                if (!this._enabled || acknowledged) return;
                acknowledged = true;
                Gio.DBus.session.emit_signal(sender, PATH, NAME, 'ClipboardConsumed', new GLib.Variant('(u)', [serial]));
            });
            global.display.get_selection().set_owner(Meta.SelectionType.CLIPBOARD, source);
            if (!source.is_active()) throw new Error('Compositor did not accept clipboard ownership');
            this._source = source;
            this._sourceOwner = sender;
            return serial;
        }, '(u)');
    }

    ReleaseAsync(_params, invocation) { this._reply(invocation, sender => this._release(sender)); }

    _grab(sender, token, accelerator) {
        const action = global.display.grab_accelerator(accelerator, Meta.KeyBindingFlags.IGNORE_AUTOREPEAT);
        if (action === Meta.KeyBindingAction.NONE) throw new Error(`Shortcut conflict or invalid accelerator: ${accelerator}`);
        Main.wm.allowKeybinding(Meta.external_binding_name_for_action(action), Shell.ActionMode.NORMAL);
        const binding = {sender, token, accelerator, action};
        this._actions.set(action, binding);
        return binding;
    }

    _ungrab(binding) {
        if (!binding.action) return;
        global.display.ungrab_accelerator(binding.action);
        Main.wm.allowKeybinding(Meta.external_binding_name_for_action(binding.action), Shell.ActionMode.NONE);
        this._actions.delete(binding.action);
        binding.action = 0;
    }

    _release(sender) {
        const client = this._clients.get(sender);
        if (!client) return;
        if (this._pending?.sender === sender) this._cancelPending();
        for (const binding of client.bindings.values()) this._ungrab(binding);
        if (client.watch) Gio.bus_unwatch_name(client.watch);
        this._clients.delete(sender);
        if (this._sourceOwner === sender && this._source) {
            global.display.get_selection().unset_owner(Meta.SelectionType.CLIPBOARD, this._source);
            this._source = this._sourceOwner = null;
        }
    }

    _activated(action) {
        const binding = this._actions.get(action);
        if (!binding || this._pending) return;
        const event = Clutter.get_current_event();
        if (!event || event.type() !== Clutter.EventType.KEY_PRESS) return;
        const actor = new St.Widget({reactive: true, can_focus: true, opacity: 0});
        Main.uiGroup.add_child(actor);
        const grab = Main.pushModal(actor, {actionMode: Shell.ActionMode.POPUP});
        if (!(grab.get_seat_state() & Clutter.GrabState.KEYBOARD)) {
            Main.popModal(grab); actor.destroy(); return;
        }
        this._pending = {...binding, actor, grab, keycode: event.get_key_code(), released: false};
        actor.connect('key-release-event', (_actor, release) => {
            if (!this._pending) return Clutter.EVENT_STOP;
            if (release.get_key_code() === this._pending.keycode) this._pending.released = true;
            // Idle after this actual release lets Mutter update its modifier
            // state. No timer guesses a physical key release.
            if (!this._releaseIdle) this._releaseIdle = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
                this._releaseIdle = 0;
                this._finishReleased();
                return GLib.SOURCE_REMOVE;
            });
            return Clutter.EVENT_STOP;
        });
        actor.connect('key-press-event', (_actor, press) => {
            if (press.get_key_symbol() === Clutter.KEY_Escape) this._cancelPending();
            return Clutter.EVENT_STOP;
        });
        actor.grab_key_focus();
    }

    _finishReleased() {
        if (!this._pending?.released) return;
        const modifiers = Clutter.ModifierType.SHIFT_MASK | Clutter.ModifierType.CONTROL_MASK |
            Clutter.ModifierType.MOD1_MASK | Clutter.ModifierType.MOD4_MASK | Clutter.ModifierType.MOD5_MASK |
            Clutter.ModifierType.SUPER_MASK | Clutter.ModifierType.META_MASK | Clutter.ModifierType.HYPER_MASK;
        if (global.get_pointer()[2] & modifiers) return;
        const {sender, token} = this._pending;
        this._cancelPending(); // Restores the previous native window focus first.
        Gio.DBus.session.emit_signal(sender, PATH, NAME, 'ShortcutReleased', new GLib.Variant('(s)', [token]));
    }

    _cancelPending() {
        if (this._releaseIdle) GLib.Source.remove(this._releaseIdle);
        this._releaseIdle = 0;
        if (!this._pending) return;
        const {grab, actor} = this._pending;
        this._pending = null;
        Main.popModal(grab);
        actor.destroy();
    }
}
