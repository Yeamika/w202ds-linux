// SPDX-License-Identifier: GPL-2.0-or-later
//
// W202DS screen lock: phone-style screen off / on on top of GNOME Shell's own screen shield.
//
//  - Screen off (power key, through ScreenOff() on D-Bus) raises the shield without its slide-down animation, so
//    the lock screen is never seen while the screen goes dark.
//  - Waking shows the lock screen clock. While the shield is not locked, a swipe up (or a key, a scroll down)
//    lifts it and returns to the session without a password; a tap does nothing, as a stray touch must not
//    unlock (GNOME's tap action also fires on the release of a failed drag). The lift is this extension's own
//    slide followed by GNOME's non-animated deactivate(false): GNOME's animated deactivate(true), which no GNOME
//    code path uses, cancels its own slide-out and leaves the shield unable to show again (the next screen off
//    then keeps an invisible, input-grabbing lock screen over a frozen-looking session).
//  - The shield locks once the screen has been off for org.gnome.desktop.screensaver lock-delay seconds (0: at
//    once; lock-enabled false: never). Wall-clock time is compared on wake as well, so time spent suspended
//    counts. With a non-zero delay a suspend no longer locks at once.
//  - Whatever lights the outputs while the shield is up (gnome-settings-daemon on a charger plug, a notification,
//    a key) also drops the shield's black curtain, so a lit screen is never left black.
//  - Screen off fades the screen to black (FADE_TIME) before the outputs go off, on the lock screen as well; another
//    ScreenOff (power key pressed again) or a ScreenOn during the fade cancels it and the screen stays on.
//  - The lock screen shows the wallpaper itself, dimmed, instead of GNOME's heavily blurred copy of it.
//  - Closing the cover (Hall lid switch, through UPower and Mutter's lid-is-closed-changed) turns the screen off
//    at once, without the fade; opening it turns the screen on to the lock screen. While the cover is closed the
//    outputs stay off whatever lights them (keys of the folded keyboard, gnome-settings-daemon).
//  - The lit lock screen, password prompt included, fades to black over IDLE_FADE_TIME before it goes dark when
//    left alone (30 s after the last input, as gnome-settings-daemon would blank it at once); input during the
//    fade lights it again.
//
// Everything else (authentication, notifications, the session's own idle fade) is GNOME's.

import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {ScreenShield} from 'resource:///org/gnome/shell/ui/screenShield.js';
import {UnlockDialog} from 'resource:///org/gnome/shell/ui/unlockDialog.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

const OBJECT_PATH = '/io/github/w202/ScreenLock';
const IFACE = `<node>
  <interface name="io.github.w202.ScreenLock">
    <method name="ScreenOff"/>
    <method name="ScreenOn"/>
  </interface>
</node>`;
const MUTTER = ['org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig', 'org.gnome.Mutter.DisplayConfig'];
const MODIFIERS = [Clutter.KEY_Shift_L, Clutter.KEY_Shift_R, Clutter.KEY_Shift_Lock, Clutter.KEY_Caps_Lock,
    Clutter.KEY_Control_L, Clutter.KEY_Control_R, Clutter.KEY_Alt_L, Clutter.KEY_Alt_R,
    Clutter.KEY_Super_L, Clutter.KEY_Super_R];
const LIFT_TIME = 300;    // ms, GNOME's CURTAIN_SLIDE_TIME
const FADE_TIME = 300;    // ms, screen off fade to black, GNOME's MANUAL_FADE_TIME
const DIM = 0.3;          // opacity of the black layer over the lock screen wallpaper
// The lit lock screen goes dark this long after the last input, as gnome-settings-daemon blanks it
// (SCREENSAVER_TIMEOUT_BLANK); after a wake that was no input of the user (outputs lit by gnome-settings-daemon
// on plugging in power, WakeUpScreen) that daemon blanks it POWER_UP_TIME_ON_AC after the wake.
const LOCK_SCREEN_IDLE_BLANK = 30000;   // ms
const LOCK_SCREEN_UNIDLE_BLANK = 15000; // ms
const IDLE_FADE_TIME = 3000;            // ms, fade to black before that blank

export default class W202ScreenLock extends Extension {
    enable() {
        const ext = this;
        this._shield = Main.screenShield;
        this._settings = new Gio.Settings({schema_id: 'org.gnome.desktop.screensaver'});
        this._offAt = 0;        // wall-clock µs when the shield came up, 0 while it is down
        this._lockTimeoutId = 0;
        this._lifting = false;  // our slide-up of the lock screen is running
        this._raiseWhenDownId = 0;
        this._litAt = 0;        // monotonic µs when the lock screen was last lit
        this._litBase = 0;      // ms after _litAt at which it goes dark when there was no input since
        this._idleBlankId = 0;  // timeout: start of the idle fade, then its end
        this._ownWakeAt = 0;    // monotonic µs of our last PowerSaveMode 0
        this._injections = new InjectionManager();
        const unlocked = () => ext._shield?.active && !ext._shield.locked;

        // Waking: keep the lock screen up (the clock) instead of returning straight to the session, after
        // locking first if the screen was off for longer than the lock delay.
        this._injections.overrideMethod(ScreenShield.prototype, '_onUserBecameActive', orig => function () {
            if (!ext._shield || !this._isActive)
                return orig.call(this);    // idle fade not finished yet: GNOME cancels it
            ext._lockIfDue();
            if (this._becameActiveId) {
                this.idleMonitor.remove_watch(this._becameActiveId);
                this._becameActiveId = 0;
            }
            this._longLightbox.lightOff();
            this._shortLightbox.lightOff();
            ext._setPowerSave(0);
            ext._armIdleBlank(LOCK_SCREEN_IDLE_BLANK);  // input, or our own wake
            return undefined;
        });

        // Suspend: with a lock delay, keep the shield up without locking; the wake check above locks if due.
        this._injections.overrideMethod(ScreenShield.prototype, '_prepareForSleep', orig => function (lm, about) {
            if (!ext._shield || !about || ext._lockDelay() <= 0)
                return orig.call(this, lm, about);
            if (!this.active)
                ext._raiseShield();
            return undefined;
        });

        // The lock screen while not locked: a key or a scroll down, which would show the password prompt, lifts
        // the shield. The tap action calls this too, on any touch or click release; only a swipe lifts for those.
        this._injections.overrideMethod(UnlockDialog.prototype, '_showPrompt', orig => function () {
            if (!unlocked())
                return orig.call(this);
            const type = Clutter.get_current_event()?.type();
            if (type === Clutter.EventType.TOUCH_END || type === Clutter.EventType.BUTTON_RELEASE)
                return undefined;
            ext._lift(LIFT_TIME);
            return undefined;
        });
        this._injections.overrideMethod(UnlockDialog.prototype, '_swipeBegin', orig => function (tracker, monitor) {
            if (!unlocked())
                return orig.call(this, tracker, monitor);
            if (monitor !== Main.layoutManager.primaryIndex)
                return undefined;
            // pick the lock screen up where it is, also in the middle of a lift
            const group = ext._shield._lockDialogGroup;
            group.remove_all_transitions();
            ext._revealSession(true);
            const progress = Math.min(1, Math.max(0, -group.translation_y / global.stage.height));
            tracker.confirmSwipe(global.stage.height, [0, 1], progress, 0);
            return undefined;
        });
        this._injections.overrideMethod(UnlockDialog.prototype, '_swipeUpdate', orig => function (tracker, progress) {
            if (!unlocked())
                return orig.call(this, tracker, progress);
            ext._shield._lockDialogGroup.translation_y = -progress * global.stage.height;
            return undefined;
        });
        this._injections.overrideMethod(UnlockDialog.prototype, '_swipeEnd', orig => function (tracker, duration, end) {
            if (!unlocked())
                return orig.call(this, tracker, duration, end);
            if (end >= 1) {
                ext._lift(duration);
            } else {
                ext._shield._lockDialogGroup.ease({
                    translation_y: 0,
                    duration,
                    mode: Clutter.AnimationMode.EASE_OUT_CUBIC,
                    onStopped: () => ext._revealSession(false),
                });
            }
            return undefined;
        });
        // Typing on the lock screen starts the password prompt in a class handler; catch keys first.
        this._injections.overrideMethod(UnlockDialog.prototype, 'open', orig => function (...args) {
            if (!this._w202KeyId) {
                this._w202KeyId = this.connect('key-press-event', (actor, event) => {
                    if (!unlocked() || MODIFIERS.includes(event.get_key_symbol()))
                        return Clutter.EVENT_PROPAGATE;
                    ext._lift(LIFT_TIME);
                    return Clutter.EVENT_STOP;
                });
            }
            return orig.apply(this, args);
        });

        // The lock screen background: the wallpaper without the blur, under a dimming layer for the clock. The blur
        // effect is removed rather than set to radius 0, which would still render the wallpaper offscreen each frame.
        this._injections.overrideMethod(UnlockDialog.prototype, '_updateBackgroundEffects', orig => function () {
            orig.call(this);
            for (const widget of this._backgroundGroup) {
                widget.remove_effect_by_name('blur');
                if (!widget._w202Dim) {
                    widget._w202Dim = new St.Widget({style: `background-color: rgba(0, 0, 0, ${DIM});`});
                    widget._w202Dim.add_constraint(new Clutter.BindConstraint({
                        source: widget, coordinate: Clutter.BindCoordinate.SIZE,
                    }));
                    widget.add_child(widget._w202Dim);
                }
            }
        });

        this._shield.connectObject('active-changed', () => this._onActiveChanged(), this);
        this._settings.connectObject('changed', () => this._armLock(), this);
        this._onActiveChanged();

        // Outputs lit while the shield's curtain is down: show the lock screen. With the cover closed, dark again.
        this._powerSaveId = Gio.DBus.session.signal_subscribe(null, 'org.freedesktop.DBus.Properties',
            'PropertiesChanged', MUTTER[1], MUTTER[2], Gio.DBusSignalFlags.NONE, (...args) => {
                const [, changed] = args[5].deepUnpack();
                const mode = changed.PowerSaveMode?.unpack();
                if (mode === undefined)
                    return;
                // our own Set is answered within a frame or two; one second leaves room for a busy compositor
                const own = GLib.get_monotonic_time() - this._ownWakeAt < 1000000;
                if (mode !== 0) {
                    this._cancelIdleBlank();
                    return;
                }
                if (this._lidClosed) {
                    this._log('cover closed: screen kept off');
                    this._setPowerSave(3);
                } else if (this._shield.active && !own) {
                    // lit by someone else (gnome-settings-daemon): GNOME's wake, which announces WakeUpScreen
                    this._shield._wakeUpScreen();
                    this._armIdleBlank(LOCK_SCREEN_UNIDLE_BLANK);
                }
            });
        // Mutter follows UPower's LidIsClosed but does not expose its state to JavaScript; read it once here and
        // follow Mutter's change signal afterwards (a signal arriving before the reply wins over the reply).
        this._lidClosed = false;
        let lidSignalled = false;
        Gio.DBus.system.call('org.freedesktop.UPower', '/org/freedesktop/UPower', 'org.freedesktop.DBus.Properties',
            'Get', new GLib.Variant('(ss)', ['org.freedesktop.UPower', 'LidIsClosed']), null,
            Gio.DBusCallFlags.NONE, -1, null, (bus, res) => {
                try {
                    const closed = bus.call_finish(res).deepUnpack()[0].unpack();
                    if (!lidSignalled && this._injections)
                        this._lidClosed = closed;
                } catch (e) {
                    this._log(`cannot read the cover state: ${e.message}`);
                }
            });
        global.backend.connectObject('lid-is-closed-changed', (backend, closed) => {
            lidSignalled = true;
            this._lidClosed = closed;
            if (closed) {
                this._cancelFade();
                this._screenOff();
                this._log('cover closed');
            } else {
                this.ScreenOn();
                this._log('cover opened');
            }
        }, this);

        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._dbus.export(Gio.DBus.session, OBJECT_PATH);
    }

    disable() {
        // Kept enabled on the lock screen (session-modes); this runs only when the extension is switched off.
        this._dbus.unexport();
        this._dbus = null;
        Gio.DBus.session.signal_unsubscribe(this._powerSaveId);
        this._shield.disconnectObject(this);
        global.backend.disconnectObject(this);
        this._settings.disconnectObject(this);
        this._disarm();
        this._cancelIdleBlank();
        if (this._raiseWhenDownId)
            this._shield.disconnect(this._raiseWhenDownId);
        this._raiseWhenDownId = 0;
        this._shield._lockDialogGroup.remove_all_transitions();
        this._shield._lockDialogGroup.translation_y = 0;
        this._revealSession(false);
        this._cancelFade();
        this._injections.clear();
        this._shield._dialog?._updateBackgrounds();  // rebuild the lock screen background as GNOME's
        this._injections = null;
        this._settings = null;
        this._shield = null;
    }

    _log(msg) {
        console.log(`${this.uuid}: ${msg}`);
    }

    _lockDelay() {
        if (!this._settings.get_boolean('lock-enabled'))
            return -1;
        return this._settings.get_uint('lock-delay');
    }

    _onActiveChanged() {
        if (this._shield.active) {
            if (!this._offAt)
                this._offAt = GLib.get_real_time();
            this._armLock();
        } else {
            this._offAt = 0;
            this._disarm();
            this._cancelIdleBlank();
        }
    }

    _armLock() {
        this._disarm();
        if (!this._offAt || this._shield.locked)
            return;
        const delay = this._lockDelay();
        if (delay < 0)
            return;
        const left = Math.max(0, delay - Math.floor((GLib.get_real_time() - this._offAt) / 1000000));
        // an idle source even for 0: active-changed is emitted from inside ScreenShield.activate()
        this._lockTimeoutId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, left, () => {
            this._lockTimeoutId = 0;
            this._lockIfDue();
            return GLib.SOURCE_REMOVE;
        });
    }

    _disarm() {
        if (this._lockTimeoutId)
            GLib.source_remove(this._lockTimeoutId);
        this._lockTimeoutId = 0;
    }

    _lockIfDue() {
        const delay = this._lockDelay();
        if (delay < 0 || !this._offAt || !this._shield.active || this._shield.locked)
            return;
        if (GLib.get_real_time() - this._offAt >= delay * 1000000) {
            this._log(`locking: screen off for ${delay} s or more`);
            this._shield.lock(false);
        }
    }

    _setPowerSave(mode) {
        if (mode === 0)
            this._ownWakeAt = GLib.get_monotonic_time();
        Gio.DBus.session.call(MUTTER[0], MUTTER[1], 'org.freedesktop.DBus.Properties', 'Set',
            new GLib.Variant('(ssv)', [MUTTER[2], 'PowerSaveMode', GLib.Variant.new_int32(mode)]),
            null, Gio.DBusCallFlags.NONE, -1, null, null);
    }

    // Raise the shield without locking, in the order lock() and the idle path use: the shield's own modal grab
    // (LOCK_SCREEN) first, then the unlock dialog's (UNLOCK_SCREEN) on top of it. In the reverse order the
    // lock-screen grab ends up on top, taking key focus from the dialog and disabling its swipe gesture.
    _raiseShield() {
        if (!this._shield._becomeModal()) {
            this._log('cannot raise the lock screen: modal grab refused');
            return false;
        }
        this._shield.activate(false);
        return true;
    }

    // D-Bus

    ScreenOff() {
        if (this._curtain) {
            // pressed again while fading out: keep the screen on
            this._cancelFade();
            this._log('screen off cancelled');
            return;
        }
        const shield = this._shield;
        if (shield.active && (shield._shortLightbox.visible || shield._longLightbox.visible)) {
            // the lock screen is up behind its black curtain already: nothing to fade
            this._screenOff();
            return;
        }
        const curtain = this._curtain = new Clutter.Actor({background_color: new Cogl.Color({alpha: 255}), opacity: 0});
        curtain.add_constraint(new Clutter.BindConstraint({source: global.stage, coordinate: Clutter.BindCoordinate.ALL}));
        Main.uiGroup.add_child(curtain);
        Main.uiGroup.set_child_above_sibling(curtain, null);
        curtain.ease({
            opacity: 255,
            duration: FADE_TIME,
            mode: Clutter.AnimationMode.EASE_IN_QUAD,
            onComplete: () => {
                this._screenOff();  // raises the shield, whose own black curtain is up at once
                this._cancelFade();
            },
        });
    }

    _cancelFade() {
        const curtain = this._curtain;
        this._curtain = null;
        curtain?.destroy();
    }

    _screenOff() {
        const shield = this._shield;
        this._cancelIdleBlank();
        if (this._lifting) {
            // a lift was under way: the lock screen stays
            shield._lockDialogGroup.remove_all_transitions();  // onStopped clears _lifting, no deactivate
            shield._lockDialogGroup.translation_y = 0;
            this._revealSession(false);
        }
        if (shield.active && Main.sessionMode.currentMode !== 'unlock-dialog') {
            // GNOME is finishing a deactivation (its last slide, ~250 ms): raise the shield again once it is down
            this._raiseWhenDown();
        } else if (shield.active) {
            // the lock screen is showing: put its black curtain back before the outputs go off
            shield._activateFade(shield._shortLightbox, 0);
        } else {
            this._raiseShield();
        }
        this._setPowerSave(3);
        this._log(`screen off${this._shield.locked ? ' (locked)' : ''}`);
    }

    ScreenOn() {
        this._cancelFade();
        if (this._shield.active) {
            // GNOME's _wakeUpScreen() without its WakeUpScreen announcement: gnome-settings-daemon answers that with
            // a blank 15 s later that no input postpones; this wake is the user's, and the lock screen goes dark
            // after LOCK_SCREEN_IDLE_BLANK with a fade (_armIdleBlank, from _onUserBecameActive).
            this._shield._onUserBecameActive();
        } else {
            this._setPowerSave(0);
        }
        this._log(`screen on${this._shield.locked ? ' (locked)' : ''}`);
    }

    // The lock screen lit: fade it to black over IDLE_FADE_TIME and blank it when it has had no input for
    // LOCK_SCREEN_IDLE_BLANK, or `base` ms after now if that is later (a wake that was no input). Input during the
    // fade reaches _onUserBecameActive (GNOME's becameActive watch of _activateFade), which lights it and re-arms.
    _armIdleBlank(base) {
        this._cancelIdleBlank();
        if (!this._shield.active)
            return;
        this._litAt = GLib.get_monotonic_time();
        this._litBase = base;
        this._scheduleIdleBlank();
    }

    _scheduleIdleBlank() {
        const now = GLib.get_monotonic_time();
        const lastInput = now - this._shield.idleMonitor.get_idletime() * 1000;
        const deadline = Math.max(this._litAt + this._litBase * 1000, lastInput + LOCK_SCREEN_IDLE_BLANK * 1000);
        const wait = Math.ceil((deadline - IDLE_FADE_TIME * 1000 - now) / 1000);
        if (wait > 50) {
            // input since arming moves the deadline: look again then
            this._idleBlankId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, wait, () => {
                this._idleBlankId = 0;
                this._scheduleIdleBlank();
                return GLib.SOURCE_REMOVE;
            });
            return;
        }
        if (!this._shield.active || this._lidClosed)
            return;
        this._shield._activateFade(this._shield._shortLightbox, IDLE_FADE_TIME);
        this._idleBlankId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, IDLE_FADE_TIME, () => {
            this._idleBlankId = 0;
            this._log('idle on the lock screen');
            this._screenOff();
            return GLib.SOURCE_REMOVE;
        });
    }

    _cancelIdleBlank() {
        if (this._idleBlankId)
            GLib.source_remove(this._idleBlankId);
        this._idleBlankId = 0;
    }

    // While the lock screen is up GNOME hides the session, the desktop background included (both are in the window
    // group, hidden in the unlock-dialog session mode). Lifting the lock screen with the finger uncovers part of the
    // stage where then nothing paints, and the screen shows whatever an earlier frame left in that buffer (streaks).
    // Show the window group, which lies below the shield, during the lift; top_window_group lies above the shield
    // and stays hidden. GNOME's own deactivation leaves the unlock-dialog mode first and needs none of this.
    _revealSession(show) {
        if (show)
            global.window_group.show();
        else
            Main.layoutManager._updateVisibility();  // back to what the session mode says
    }

    // Slide the lock screen up from where it is, then let GNOME take the shield down without its own animation.
    _lift(duration) {
        const group = this._shield._lockDialogGroup;
        this._lifting = true;
        group.remove_all_transitions();
        this._revealSession(true);
        group.ease({
            translation_y: -global.stage.height,
            duration,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onStopped: () => {
                this._lifting = false;
            },
            onComplete: () => this._shield.deactivate(false),
        });
    }

    _raiseWhenDown() {
        if (this._raiseWhenDownId)
            return;
        this._raiseWhenDownId = this._shield.connect('active-changed', () => {
            if (this._shield.active)
                return;
            this._shield.disconnect(this._raiseWhenDownId);
            this._raiseWhenDownId = 0;
            this._raiseShield();
        });
    }
}
