// SPDX-License-Identifier: GPL-2.0-or-later
//
// W202DS power menu: a GNOME Shell dialog offering Power Off, Restart and Suspend. w202-power-key opens it over
// D-Bus when the power key is held. The dialog is Shell's own ModalDialog; the buttons run Shell's SystemActions,
// so gnome-session and logind treat them like the system menu entries (application inhibitors, lockdown and the
// lock screen rules included).

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Gettext from 'gettext';

import * as Dialog from 'resource:///org/gnome/shell/ui/dialog.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as ModalDialog from 'resource:///org/gnome/shell/ui/modalDialog.js';
import * as GnomeSession from 'resource:///org/gnome/shell/misc/gnomeSession.js';
import * as SystemActions from 'resource:///org/gnome/shell/misc/systemActions.js';
import {Extension, gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

// labels GNOME Shell already translates
const shell = Gettext.domain('gnome-shell');

const OBJECT_PATH = '/io/github/w202/PowerMenu';
const IFACE = `<node>
  <interface name="io.github.w202.PowerMenu">
    <method name="Open"/>
    <method name="Close"/>
    <property name="IsOpen" type="b" access="read"/>
  </interface>
</node>`;

// gnome-session answers Shutdown and Reboot with Shell's end-session dialog, a second confirmation with a 60 s
// countdown. For a choice made in this menu it is confirmed at once, unless it has something to show: applications
// inhibiting logout, or a pending offline update (then the dialog type differs). Types and signals are those of
// ui/endSessionDialog.js.
const END_SESSION = {
    'power-off': {type: 1, signal: 'ConfirmedShutdown'},
    'restart': {type: 2, signal: 'ConfirmedReboot'},
};
const CONFIRM_WINDOW = 30; // s from the request to gnome-session showing its dialog

const PowerMenuDialog = GObject.registerClass(
class PowerMenuDialog extends ModalDialog.ModalDialog {
    _init(actions, run) {
        // the width of Shell's own power off / restart dialog
        super._init({styleClass: 'end-session-dialog'});

        const content = new Dialog.MessageDialogContent({title: shell.gettext('Power Off Menu')});
        if (Main.sessionMode.isLocked) {
            // GNOME offers neither power off nor restart on the lock screen; suspend stays available
            content.add_child(new St.Label({
                style_class: 'message-dialog-description',
                text: _('Unlock to power off or restart'),
            }));
        }
        this.contentLayout.add_child(content);

        const cancel = this.addButton({
            label: shell.gettext('Cancel'),
            action: () => run('cancel'),
            key: Clutter.KEY_Escape,
        });
        const buttons = [
            ['can-suspend', () => actions.canSuspend, shell.gettext('Suspend'), 'suspend'],
            ['can-restart', () => actions.canRestart, shell.pgettext('button', 'Restart'), 'restart'],
            ['can-power-off', () => actions.canPowerOff, shell.pgettext('button', 'Power Off'), 'power-off'],
        ];
        for (const [property, available, label, id] of buttons) {
            const button = this.addButton({label, action: () => run(id)});
            const sync = () => {
                button.reactive = button.can_focus = available();
            };
            actions.connectObject(`notify::${property}`, sync, this);
            sync();
        }
        this.setInitialKeyFocus(cancel);
    }
});

export default class W202PowerMenu extends Extension {
    enable() {
        this._actions = SystemActions.getDefault();
        this._session = new GnomeSession.SessionManager();
        this._dialog = null;
        this._confirmTimeoutId = 0;
        this._confirmIdleId = 0;

        // A short press of the key while the menu is open turns the screen off: the menu goes with it. Mutter 48
        // exposes the power save mode to JavaScript only as its D-Bus property (PowerSaveMode > 0: outputs off).
        this._displayConfigId = Gio.DBus.session.signal_subscribe(null,
            'org.freedesktop.DBus.Properties', 'PropertiesChanged', '/org/gnome/Mutter/DisplayConfig',
            'org.gnome.Mutter.DisplayConfig', Gio.DBusSignalFlags.NONE, (...args) => {
                const [, changed] = args[5].deepUnpack();
                if (changed.PowerSaveMode?.unpack() > 0)
                    this._close('screen off');
            });
        Main.screenShield?.connectObject('active-changed', () => {
            if (Main.screenShield.active)
                this._close('screen shield');
        }, this);

        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._dbus.export(Gio.DBus.session, OBJECT_PATH);
    }

    disable() {
        this._dbus.unexport();
        this._dbus = null;
        Gio.DBus.session.signal_unsubscribe(this._displayConfigId);
        Main.screenShield?.disconnectObject(this);
        this._disarm();
        if (this._confirmIdleId)
            GLib.source_remove(this._confirmIdleId);
        this._confirmIdleId = 0;
        this._dialog?.destroy();
        this._dialog = null;
        this._session = null;
        this._actions = null;
    }

    _log(msg) {
        console.log(`${this.uuid}: ${msg}`);
    }

    get _isOpen() {
        const state = this._dialog?.state;
        return state === ModalDialog.State.OPENING || state === ModalDialog.State.OPENED;
    }

    // D-Bus

    Open() {
        if (this._isOpen)
            return;
        // Wake an active screen shield as GNOME does on resume, or else its black curtain covers the menu: a locked
        // shield drops the curtain and shows the lock screen, one that is not locked (raised without locking)
        // returns to the desktop. ScreenShield emits WakeUpScreen, and gnome-settings-daemon lights the outputs.
        if (Main.screenShield?.active)
            Main.screenShield._wakeUpScreen();
        this._actions.forceUpdate();
        const dialog = new PowerMenuDialog(this._actions, id => this._run(id));
        dialog.connect('closed', () => {
            if (this._dialog === dialog)
                this._dialog = null;
        });
        if (!dialog.open()) {
            dialog.destroy();
            this._log('cannot open: keyboard and pointer grab failed');
            throw new Error('cannot grab keyboard and pointer');
        }
        this._dialog = dialog;
        this._log(`opened${Main.sessionMode.isLocked ? ' on the lock screen' : ''}`);
    }

    Close() {
        this._close('D-Bus Close');
    }

    get IsOpen() {
        return this._isOpen;
    }

    // actions

    _close(why) {
        if (!this._isOpen)
            return;
        this._log(`closed (${why})`);
        this._dialog.close();
    }

    _run(id) {
        this._dialog?.close();
        this._log(id);
        try {
            switch (id) {
            case 'suspend':
                this._actions.activateSuspend();
                break;
            case 'restart':
            case 'power-off':
                this._endSession(id).catch(e => this._log(`${id} failed: ${e.message}`));
                break;
            }
        } catch (e) {
            this._log(`${id} failed: ${e.message}`);
        }
    }

    async _endSession(id) {
        let inhibited = true;
        try {
            [inhibited] = await this._session.IsInhibitedAsync(GnomeSession.InhibitFlags.LOGOUT);
        } catch (e) {
            this._log(`cannot query logout inhibitors: ${e.message}`);
        }
        if (inhibited)
            this._log('logout is inhibited: the end-session dialog asks');
        else
            this._armConfirm(END_SESSION[id]);
        try {
            if (id === 'power-off')
                this._actions.activatePowerOff();
            else
                this._actions.activateRestart();
        } catch (e) {
            this._disarm();
            throw e;
        }
    }

    _armConfirm({type, signal}) {
        this._disarm();
        const dialog = Main.endSessionDialog;
        dialog.connectObject('opened', () => {
            this._disarm();
            if (dialog._type !== type || dialog._applications.length > 0 || dialog._checkBox.visible) {
                this._log('end-session dialog has more to show: left to the user');
                return;
            }
            // after the dialog's own 'opened' handler has answered gnome-session's Open call
            this._confirmIdleId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                this._confirmIdleId = 0;
                this._log(`end-session dialog confirmed: ${signal}`);
                dialog._confirm(signal).catch(e => this._log(`confirm failed: ${e.message}`));
                return GLib.SOURCE_REMOVE;
            });
        }, this);
        this._confirmTimeoutId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, CONFIRM_WINDOW, () => {
            this._confirmTimeoutId = 0;
            this._disarm();
            return GLib.SOURCE_REMOVE;
        });
    }

    _disarm() {
        Main.endSessionDialog?.disconnectObject(this);
        if (this._confirmTimeoutId)
            GLib.source_remove(this._confirmTimeoutId);
        this._confirmTimeoutId = 0;
    }
}
