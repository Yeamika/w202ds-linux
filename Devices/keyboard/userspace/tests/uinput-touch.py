#!/usr/bin/python3
# Acceptance helper (root): a uinput touchscreen with the panel's geometry (1200x1920 portrait, like
# himax-touchscreen). Usage: uinput-touch.py X0 Y0 X1 Y1 [ms]  (one finger, straight line) | uinput-touch.py tap X Y
# Coordinates are the panel's; in landscape a swipe up runs from small to large X, e.g. 200 960 1000 960 350.
import fcntl, os, struct, sys, time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TOUCH = 0x14a
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39
W, H = 1200, 1920
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x401C5504, 0x5501, 0x5502

fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
for ev in (EV_KEY, EV_ABS):
    fcntl.ioctl(fd, UI_SET_EVBIT, ev)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
fcntl.ioctl(fd, UI_SET_PROPBIT, 1)  # INPUT_PROP_DIRECT
for code, maximum in ((ABS_X, W - 1), (ABS_Y, H - 1), (ABS_MT_SLOT, 9), (ABS_MT_POSITION_X, W - 1),
                      (ABS_MT_POSITION_Y, H - 1), (ABS_MT_TRACKING_ID, 65535)):
    fcntl.ioctl(fd, UI_SET_ABSBIT, code)
    fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack("HH6i", code, 0, 0, 0, maximum, 0, 0, 10))
fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("HHHH80sI", 0x18, 0x1234, 0x567c, 1, b"w202-touch-test", 0))
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(1.5)  # let libinput and mutter add and map the device


def emit(t, c, v):
    os.write(fd, struct.pack("llHHi", 0, 0, t, c, v))


def frame(x, y):
    emit(EV_ABS, ABS_MT_POSITION_X, x); emit(EV_ABS, ABS_MT_POSITION_Y, y)
    emit(EV_ABS, ABS_X, x); emit(EV_ABS, ABS_Y, y); emit(EV_SYN, 0, 0)


def down(x, y):
    emit(EV_ABS, ABS_MT_SLOT, 0); emit(EV_ABS, ABS_MT_TRACKING_ID, 1); emit(EV_KEY, BTN_TOUCH, 1); frame(x, y)


def up():
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1); emit(EV_KEY, BTN_TOUCH, 0); emit(EV_SYN, 0, 0)


if sys.argv[1] == "tap":
    down(int(sys.argv[2]), int(sys.argv[3])); time.sleep(0.06); up()
else:
    x0, y0, x1, y1 = map(int, sys.argv[1:5])
    ms = int(sys.argv[5]) if len(sys.argv) > 5 else 300
    hold = float(sys.argv[6]) if len(sys.argv) > 6 else 0  # seconds to keep the finger down at (x1, y1)
    steps = max(2, ms // 8)
    down(x0, y0)
    for i in range(1, steps + 1):
        time.sleep(ms / 1000 / steps)
        frame(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
    time.sleep(hold)
    up()
time.sleep(0.3)
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
