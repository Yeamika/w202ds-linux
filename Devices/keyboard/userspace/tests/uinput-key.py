#!/usr/bin/python3
"""Tap one key on a temporary uinput keyboard, to drive the power menu without touching the tablet.

Run as root: uinput-key.py KEYCODE (Linux input code, e.g. 1 = Esc, 15 = Tab, 28 = Enter).
"""
import fcntl
import os
import struct
import sys
import time

UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
EV_SYN, EV_KEY, BUS_VIRTUAL = 0, 1, 0x06
EVENT = struct.Struct("llHHi")  # struct input_event on arm64


def main(code):
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("HHHH80sI", BUS_VIRTUAL, 0, 0, 1, b"w202 test keyboard", 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1.0)  # the compositor adds the new device
    for value in (1, 0):
        os.write(fd, EVENT.pack(0, 0, EV_KEY, code, value))
        os.write(fd, EVENT.pack(0, 0, EV_SYN, 0, 0))
        time.sleep(0.05)
    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(int(sys.argv[1]))
