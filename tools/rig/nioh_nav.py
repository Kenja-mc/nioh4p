#!/usr/bin/env python3
"""Nioh rig navigation harness: classify screen state and drive menu transitions.

Slots a/b/c resolve their display and compositor socket from the live game's environment.

usage:
  nioh_nav.py -s a shot [name]     capture a screenshot into shots/ (waits for file)
  nioh_nav.py -s a classify         print state id + evidence
  nioh_nav.py -s a wait STATE [s]   poll until classify == STATE
  nioh_nav.py -s a tap KEY...       send 80 ms key taps (xkey-style, in-process)
  nioh_nav.py -s a walk SECONDS     hold W (forward walk)
  nioh_nav.py -s a ocr [X Y W H]    OCR full frame or crop (preprocessed)
  nioh_nav.py -s a goto STATE       scripted, verified transition
  nioh_nav.py -s a map_select SUBSTR  on world map: move node cursor until the right
                                    panel mentions SUBSTR, then OK

States: boot_notice logo press_any title_menu dark lore_continue world_map
        mission_brief torii_menu yokai_params searching in_mission dialog_yesno
        shrine_menu unknown
"""
import argparse
import ctypes
import os
import re
import subprocess
import sys
import time

RIG = os.path.expanduser("~/nioh3p-test")
SLOTS = {
    slot: {"display": None, "rundir": f"{RIG}/run-{slot}", "wayland": "gamescope-0"}
    for slot in ("a", "b", "c", "d")
}
SHOTS = f"{RIG}/shots"


def resolve_slot(slot):
    """Use the live game's endpoints, not Xwayland's launch-order numbers."""
    matches = []
    for proc in os.scandir("/proc"):
        if not proc.name.isdigit():
            continue
        try:
            with open(f"{proc.path}/comm") as name:
                if name.read().strip() != "nioh.exe":
                    continue
            with open(f"{proc.path}/environ", "rb") as environ:
                env = dict(entry.split("=", 1) for entry in
                           environ.read().decode(errors="replace").split("\0") if "=" in entry)
        except OSError:
            continue
        if env.get("HOME") == f"{RIG}/home-{slot}" and env.get("DISPLAY"):
            matches.append(env)
    if len(matches) > 1:
        sys.exit(f"nioh_nav: multiple live games for slot {slot}; stop the duplicate first")
    if not matches:
        return False
    env = matches[0]
    SLOTS[slot].update(display=env["DISPLAY"],
                       rundir=env.get("XDG_RUNTIME_DIR", SLOTS[slot]["rundir"]),
                       wayland=env.get("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0"))
    return True

# ---- input (libX11 + libXtst, same approach as tools/rig/xkey.py) ----
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


class Input:
    def __init__(self, display):
        self.dpy = x11.XOpenDisplay(display.encode())
        if not self.dpy:
            sys.exit(f"nioh_nav: cannot open display {display}")

    def _code(self, name):
        c = x11.XKeysymToKeycode(self.dpy, x11.XStringToKeysym(name.encode()))
        if not c:
            sys.exit(f"nioh_nav: unknown keysym {name}")
        return c

    def tap(self, name, ms=90):
        c = self._code(name)
        xtst.XTestFakeKeyEvent(self.dpy, c, True, 0)
        x11.XFlush(self.dpy)
        time.sleep(ms / 1000.0)
        xtst.XTestFakeKeyEvent(self.dpy, c, False, 0)
        x11.XFlush(self.dpy)
        time.sleep(0.06)

    def hold(self, name, seconds):
        c = self._code(name)
        xtst.XTestFakeKeyEvent(self.dpy, c, True, 0)
        x11.XFlush(self.dpy)
        time.sleep(seconds)
        xtst.XTestFakeKeyEvent(self.dpy, c, False, 0)
        x11.XFlush(self.dpy)


# ---- capture ----
def shot(slot, name=None, settle=2.0):
    name = name or f"nav-{slot}-{int(time.time())}.png"
    path = os.path.join(SHOTS, name)
    if os.path.exists(path):
        os.unlink(path)
    time.sleep(settle)  # settle the UI before choosing the captured frame
    env = dict(os.environ,
               XDG_RUNTIME_DIR=SLOTS[slot]["rundir"],
               GAMESCOPE_WAYLAND_DISPLAY=SLOTS[slot]["wayland"])
    subprocess.run(["gamescopectl", "screenshot", path], env=env,
                   capture_output=True, timeout=15)
    deadline = time.time() + 10
    while time.time() < deadline:
        if os.path.exists(path) and os.path.getsize(path) >= 12:
            with open(path, "rb") as image:
                image.seek(-12, os.SEEK_END)
                complete = image.read() == b"\x00\x00\x00\x00IEND\xaeB\x60\x82"
            if complete:
                return path
        time.sleep(0.3)
    return None


# ---- OCR / classify ----
def ocr(path, crop=None):
    src = path
    tmp = "/tmp/nav_ocr.png"
    if crop:
        x, y, w, h = crop
        subprocess.run(["magick", path, "-crop", f"{w}x{h}+{x}+{y}", "+repage",
                        "-resize", "300%", "-colorspace", "Gray", "-sharpen", "0x1", tmp],
                       check=True, capture_output=True)
        src = tmp
    else:
        subprocess.run(["magick", path, "-resize", "200%", "-colorspace", "Gray",
                        "-sharpen", "0x1", tmp], check=True, capture_output=True)
        src = tmp
    r = subprocess.run(["tesseract", src, "-", "--psm", "6"],
                       capture_output=True, text=True)
    return " ".join(r.stdout.split())


def mean_luma(path):
    r = subprocess.run(["magick", path, "-colorspace", "Gray",
                        "-format", "%[fx:mean]", "info:"],
                       capture_output=True, text=True)
    try:
        return float(r.stdout)
    except ValueError:
        return 1.0


RULES = [
    ("boot_notice", ["autosave"]),
    ("press_any", ["press any", "any button"]),
    ("title_menu", ["load game", "new game"]),
    ("mission_brief", ["begin mission", "become a visitor"]),
    ("torii_menu", ["yokai realm with a companion", "random encounters"]),
    ("yokai_params", ["without conditions", "with conditions"]),
    ("searching", ["searching for a companion", "searching for", "cancel"]),
    ("world_map", ["starting point", "twilight mission"]),
    ("lore_continue", ["continue"]),
    ("dialog_yesno", ["yes", "no"]),
]
HUD_RE = re.compile(r"\b\d{2,4}\s*/\s*\d{2,4}\b")


def classify(slot, verbose=False):
    path = shot(slot)
    if not path:
        return "no_signal", "no screenshot"
    text = ocr(path)
    low = text.lower()
    for state, kws in RULES:
        if any(k in low for k in kws):
            return state, text[:160]
    tl = ocr(path, crop=(0, 0, 420, 120))
    if HUD_RE.search(tl):
        return "in_mission", f"tl-hud: {tl[:80]}"
    # stylized map footer is small; crop before giving up
    footer = ocr(path, crop=(0, 610, 1280, 110)).lower()
    if "starting point" in footer or "twilight" in footer:
        return "world_map", f"footer: {footer[:80]}"
    lowf = footer
    if "region" in low and "level" not in low:
        return "world_map", f"region-title: {low[:60]}"
    lum = mean_luma(path)
    if lum < 0.055:
        return "dark", f"luma={lum:.3f}"
    if "koei tecmo" in low or "all rights" in low:
        return "logo", text[:160]
    if verbose:
        print(f"[verbose] luma={lum:.3f} text={text[:200]} tl={tl[:80]}")
    return "unknown", text[:160]


def wait_state(slot, state, timeout=120):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last, ev = classify(slot)
        if last == state:
            print(f"wait: {state} reached")
            return True
        time.sleep(2.5)
    print(f"wait: timeout, last={last} ({ev})")
    return False


# ---- transitions ----
def goto(slot, target):
    for _ in range(40):
        state, ev = classify(slot)
        if state == target:
            print(f"goto: at {target}")
            return True
        inp = Input(SLOTS[slot]["display"])
        if state in ("boot_notice", "logo", "press_any", "lore_continue"):
            inp.tap("Return")
        elif state == "dark":
            time.sleep(4)
        elif state == "title_menu" and target in ("world_map", "mission_brief", "torii_menu", "yokai_params", "searching"):
            inp.tap("Return")  # CONTINUE
        elif state == "dialog_yesno":
            inp.tap("BackSpace")
        else:
            print(f"goto: no script from {state} to {target} (ev: {ev})")
            return False
        time.sleep(3)
    return False


DIRS = ["Up", "Right", "Down", "Left"]


def map_select(slot, substr, max_steps=24):
    """Move the world-map node cursor until the right panel mentions substr."""
    inp = Input(SLOTS[slot]["display"])
    for i in range(max_steps):
        path = shot(slot, name=f"mapsel-{slot}.png")
        panel = ocr(path, crop=(980, 120, 300, 480))
        if substr.lower() in panel.lower():
            print(f"map_select: {substr} selected -> OK")
            inp.tap("Return")
            return True
        inp.tap(DIRS[i % 4] if i < 8 else DIRS[(i // 4) % 4])
        # first pass: probe each direction once, then spiral
        time.sleep(1.2)
    print("map_select: not found")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--slot", required=True, choices=SLOTS)
    ap.add_argument("cmd", choices=["shot", "classify", "wait", "tap", "walk",
                                    "ocr", "goto", "map_select"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    if not resolve_slot(a.slot) and a.cmd != "shot":
        ap.error(f"no live nioh.exe for slot {a.slot}")
    if a.cmd == "shot":
        p = shot(a.slot, a.args[0] if a.args else None)
        print(p)
    elif a.cmd == "classify":
        s, ev = classify(a.slot)
        print(f"{s}\t{ev}")
    elif a.cmd == "wait":
        ok = wait_state(a.slot, a.args[0], int(a.args[1]) if len(a.args) > 1 else 120)
        sys.exit(0 if ok else 1)
    elif a.cmd == "tap":
        inp = Input(SLOTS[a.slot]["display"])
        for k in a.args:
            inp.tap(k)
            time.sleep(0.4)
        print("tapped", " ".join(a.args))
    elif a.cmd == "walk":
        Input(SLOTS[a.slot]["display"]).hold("w", float(a.args[0]))
        print(f"walked {a.args[0]}s")
    elif a.cmd == "ocr":
        p = shot(a.slot)
        crop = tuple(int(v) for v in a.args) if len(a.args) == 4 else None
        print(ocr(p, crop))
    elif a.cmd == "goto":
        ok = goto(a.slot, a.args[0])
        sys.exit(0 if ok else 1)
    elif a.cmd == "map_select":
        ok = map_select(a.slot, a.args[0])
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
