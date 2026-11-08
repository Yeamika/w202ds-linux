#!/usr/bin/python3
"""Capture the whole screen through GNOME Shell's screenshot service, to check the power menu without a camera.

Run as the graphical session's user with its DBUS_SESSION_BUS_ADDRESS: shell-screenshot.py FILE.png
GNOME Shell only takes screenshots for allow-listed bus names; for the duration of this test it claims
org.gnome.Screenshot, the name of gnome-screenshot, and fails if that name is already owned.
"""
import sys
import time

from gi.repository import Gio, GLib

DBUS = ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus")
SCREENSHOT = ("org.gnome.Shell.Screenshot", "/org/gnome/Shell/Screenshot", "org.gnome.Shell.Screenshot")
REQUEST_NAME_DO_NOT_QUEUE, PRIMARY_OWNER = 4, 1


def call(bus, target, method, args, reply):
    return bus.call_sync(*target, method, args, GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 10000, None)


def main(path):
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    (owned,) = call(bus, DBUS, "RequestName",
                    GLib.Variant("(su)", ("org.gnome.Screenshot", REQUEST_NAME_DO_NOT_QUEUE)), "(u)").unpack()
    if owned != PRIMARY_OWNER:
        sys.exit("org.gnome.Screenshot is owned by another client")
    time.sleep(0.5)  # GNOME Shell learns the new owner from NameOwnerChanged
    ok, used = call(bus, SCREENSHOT, "Screenshot", GLib.Variant("(bbs)", (False, False, path)), "(bs)").unpack()
    if not ok:
        sys.exit("screenshot failed")
    print(used)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
