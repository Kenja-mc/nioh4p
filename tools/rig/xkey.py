#!/usr/bin/env python3
"""Input into the rig display without xdotool: libX11 + libXtst through ctypes.

usage (DISPLAY is taken from the environment, so `rig_env`-style export works):
    xkey.py key <keysym> [<keysym> ...]   e.g. xkey.py key Return Down space
    xkey.py type <text>                   letters, digits and space
    xkey.py move <x> <y>
    xkey.py click <button>                button 1 = left
    xkey.py sleep <seconds>

Keysyms are X11 names ("Return", "Tab", "Escape", "F1", "Down", "a", "Z", "1").
"""
import ctypes
import sys
import time

x11 = ctypes.CDLL("libX11.so.6")
xtst = ctypes.CDLL("libXtst.so.6")

x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XFlush.argtypes = [ctypes.c_void_p]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]

dpy = x11.XOpenDisplay(None)
if not dpy:
    sys.exit("xkey: cannot open display")

SHIFT = x11.XKeysymToKeycode(dpy, x11.XStringToKeysym(b"Shift_L"))


def tap(keysym_name, shift=False):
    code = x11.XKeysymToKeycode(dpy, x11.XStringToKeysym(keysym_name.encode()))
    if not code:
        sys.exit("xkey: unknown keysym %r" % keysym_name)
    if shift:
        xtst.XTestFakeKeyEvent(dpy, SHIFT, True, 0)
    xtst.XTestFakeKeyEvent(dpy, code, True, 0)
    # Nioh polls key state; down/up in one flush can fall between frames.
    x11.XFlush(dpy)
    time.sleep(0.08)
    xtst.XTestFakeKeyEvent(dpy, code, False, 0)
    if shift:
        xtst.XTestFakeKeyEvent(dpy, SHIFT, False, 0)
    x11.XFlush(dpy)
    time.sleep(0.05)


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    if cmd == "key":
        for k in args:
            tap(k)
    elif cmd == "type":
        for ch in " ".join(args):
            if ch == " ":
                tap("space")
            elif ch.isupper():
                tap(ch.lower(), shift=True)
            else:
                tap(ch)
    elif cmd == "move":
        x, y = int(args[0]), int(args[1])
        xtst.XTestFakeMotionEvent(dpy, -1, x, y, 0)
        x11.XFlush(dpy)
    elif cmd == "click":
        button = int(args[0]) if args else 1
        xtst.XTestFakeButtonEvent(dpy, button, True, 0)
        xtst.XTestFakeButtonEvent(dpy, button, False, 0)
        x11.XFlush(dpy)
    elif cmd == "sleep":
        time.sleep(float(args[0]))
    else:
        sys.exit(__doc__)


main(sys.argv[1:])
