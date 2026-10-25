#!/usr/bin/env python3
"""Rig lifecycle in one place: status, up, down, deploy, ini, collect.

usage:
  rig.py status                     rig processes per slot (HOME under $RIG only)
  rig.py up b a c [--client]        start the lan games one after another; each
                                    must show a frame before the next starts
  rig.py down [b a c] [--client]    stop the games (and the slot b Steam client
                                    with --client); every PID is matched by its
                                    rig HOME first
  rig.py deploy research a b        copy build/research/dinput8.dll (or player:
                                    build/dinput8.dll) into lan-<slot>
  rig.py ini a trace=1 dump_image=1 set keys in lan-<slot>/nioh3p.ini
  rig.py collect NAME               copy each slot's logs, the launch output and
                                    the newest coop trace to $RIG/runs/NAME

Processes that belong to the user's own Steam (HOME=/home/<you>) never match:
ownership is decided by the HOME in /proc/<pid>/environ, never by a name
pattern.
"""
import argparse
import glob
import hashlib
import os
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import coop  # noqa: E402
from nioh_nav import RIG, SHOTS, resolve_slot, shot  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUN_SH = os.path.join(REPO, "tools", "rig", "run.sh")
SLOTS = ("a", "b", "c", "d")
GAME_MARKER = "NIOH3P_HEADLESS"   # run.sh lan exports it; the Steam client lacks it
BUILDS = {"player": os.path.join(REPO, "build", "dinput8.dll"),
          "research": os.path.join(REPO, "build", "research", "dinput8.dll")}


# ---- processes ----
def _environ(pid):
    try:
        with open(f"/proc/{pid}/environ", "rb") as handle:
            raw = handle.read().decode(errors="replace")
    except OSError:
        return None
    return dict(item.split("=", 1) for item in raw.split("\0") if "=" in item)


def _comm(pid):
    try:
        with open(f"/proc/{pid}/comm") as handle:
            return handle.read().strip()
    except OSError:
        return "?"


def rig_processes():
    """[(pid, slot, comm, is_game)] for every process whose HOME is a rig home."""
    found = []
    me = os.getpid()
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit() or int(entry.name) == me:
            continue
        env = _environ(entry.name)
        if not env:
            continue
        home = env.get("HOME", "")
        for slot in SLOTS:
            if home == os.path.join(RIG, f"home-{slot}"):
                found.append((int(entry.name), slot, _comm(entry.name),
                              GAME_MARKER in env))
                break
    return found


def game_pids(slot):
    return [pid for pid, owner, _, game in rig_processes() if owner == slot and game]


def client_pids():
    return [pid for pid, owner, _, game in rig_processes() if owner == "b" and not game]


def _signal_all(pids, sig):
    for pid in pids:
        try:
            os.kill(pid, sig)
        except ProcessLookupError:
            pass


def _stop(label, pick, grace=15.0):
    """TERM the processes ``pick()`` returns, wait, then KILL what is left."""
    pids = pick()
    if not pids:
        print(f"rig: {label}: nothing running")
        return
    # nioh.exe first: the compositor and Wine services follow it down
    first = [pid for pid in pids if _comm(pid) == "nioh.exe"] or pids
    _signal_all(first, signal.SIGTERM)
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline and any(_comm(pid) == "nioh.exe" for pid in pick()):
        time.sleep(0.5)
    _signal_all(pick(), signal.SIGTERM)
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline and pick():
        time.sleep(0.5)
    left = pick()
    _signal_all(left, signal.SIGKILL)
    print(f"rig: {label}: stopped {len(pids)} process(es)"
          + (f", {len(left)} needed SIGKILL" if left else ""))


# ---- commands ----
def cmd_status(_args):
    rows = rig_processes()
    if not rows:
        print("rig: no rig processes")
        return 0
    for slot in SLOTS:
        mine = [row for row in rows if row[1] == slot]
        if not mine:
            continue
        games = sorted({comm for _, _, comm, game in mine if game})
        others = sorted({comm for _, _, comm, game in mine if not game})
        live = "nioh.exe" in games
        print(f"slot {slot}: game {'UP' if live else 'down'} "
              f"({len([r for r in mine if r[3]])} procs: {', '.join(games) or '-'})"
              + (f"; client procs: {', '.join(others)}" if others else ""))
    return 0


def _xvfb_running(display):
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            with open(f"{entry.path}/cmdline", "rb") as handle:
                argv = handle.read().split(b"\0")
        except OSError:
            continue
        if argv and os.path.basename(argv[0]) == b"Xvfb" and display.encode() in argv:
            return True
    return False


def _start_client():
    if client_pids():
        print("rig: slot b Steam client already running")
        return True
    if not _xvfb_running(":10"):
        subprocess.Popen(["Xvfb", ":10", "-screen", "0", "1280x720x24", "-nolisten", "tcp"],
                         stdout=open(os.path.join(RIG, "xvfb10.log"), "w"),
                         stderr=subprocess.STDOUT, start_new_session=True)
        time.sleep(2)
    out = open(os.path.join(RIG, "client-b.out"), "w")
    subprocess.Popen([RUN_SH, "b", "client"], stdout=out, stderr=subprocess.STDOUT,
                     start_new_session=True)
    print("rig: started the slot b Steam client; make sure that Friends shows "
          "Invisible before an authenticated test")
    time.sleep(20)
    return bool(client_pids())


def _wait_ready(slot, started_at, timeout):
    """The proxy rotates its log at launch and writes ``preflight:`` once its
    hooks are in place; then the compositor must give a frame."""
    log = os.path.join(coop.slot_log_dir(slot), "nioh3p.log")
    deadline = time.monotonic() + timeout
    launched = time.monotonic()
    while time.monotonic() < deadline:
        if not game_pids(slot) and time.monotonic() - launched > 15:
            return "exited"
        try:
            fresh = os.path.getmtime(log) >= started_at
            with open(log, "rb") as handle:
                ready = fresh and b"preflight:" in handle.read(65536)
        except OSError:
            ready = False
        if ready and resolve_slot(slot) and shot(slot, f"up-{slot}.png", settle=0.0):
            return "ready"
        time.sleep(2)
    return "timeout"


def cmd_up(args):
    if args.client and not _start_client():
        print("rig: the Steam client did not start", file=sys.stderr)
        return 1
    status = 0
    for slot in args.slots:
        if game_pids(slot):
            print(f"rig: slot {slot}: already running")
            continue
        out = open(os.path.join(RIG, f"game-{slot}.out"), "w")
        started_at = time.time()
        subprocess.Popen([RUN_SH, slot, "lan"], stdout=out, stderr=subprocess.STDOUT,
                         start_new_session=True, cwd=REPO)
        started = time.monotonic()
        result = _wait_ready(slot, started_at, args.timeout)
        print(f"rig: slot {slot}: {result} after {time.monotonic() - started:.0f} s"
              + (f" (log {out.name})" if result != "ready" else ""))
        if result != "ready":
            status = 1
    return status


def cmd_down(args):
    for slot in args.slots or SLOTS:
        _stop(f"slot {slot}", lambda slot=slot: game_pids(slot))
    if args.client:
        _stop("slot b Steam client", client_pids)
    return 0


def _sha(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def cmd_deploy(args):
    source = BUILDS[args.build]
    if not os.path.exists(source):
        print(f"rig: {source} is missing: run ./build.sh", file=sys.stderr)
        return 1
    for slot in args.slots:
        if game_pids(slot):
            print(f"rig: slot {slot} is running: stop it first", file=sys.stderr)
            return 1
        target = os.path.join(RIG, f"lan-{slot}", "dinput8.dll")
        shutil.copy2(source, target)
        print(f"rig: slot {slot}: {args.build} {_sha(target)[:8]}")
    return 0


def cmd_ini(args):
    path = os.path.join(RIG, f"lan-{args.slot}", "nioh3p.ini")
    wanted = {}
    for item in args.pairs:
        key, sep, value = item.partition("=")
        if not sep or not key.strip():
            print(f"rig: expected key=value, got {item!r}", file=sys.stderr)
            return 2
        wanted[key.strip()] = value.strip()
    with open(path, encoding="utf-8", errors="replace", newline="") as handle:
        text = handle.read()
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines()
    done = set()
    for index, line in enumerate(lines):
        key = line.partition("=")[0].strip()
        if key in wanted and not line.lstrip().startswith((";", "#")):
            lines[index] = f"{key}={wanted[key]}"
            done.add(key)
    lines += [f"{key}={value}" for key, value in wanted.items() if key not in done]
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(newline.join(lines) + newline)
    print(f"rig: slot {args.slot}: " + " ".join(f"{k}={v}" for k, v in wanted.items()))
    return 0


def cmd_collect(args):
    target = os.path.join(RIG, "runs", args.name)
    os.makedirs(target, exist_ok=True)
    copied = []
    for slot in SLOTS:
        log_dir = coop.slot_log_dir(slot)
        for name in ("nioh3p.log", "nioh3p.prev.log"):
            path = os.path.join(log_dir, name)
            if os.path.exists(path) and os.path.isdir(os.path.join(RIG, f"lan-{slot}")):
                dest = os.path.join(target, f"{slot}-{name}")
                shutil.copy2(path, dest)
                copied.append(dest)
        out = os.path.join(RIG, f"game-{slot}.out")
        if os.path.exists(out):
            shutil.copy2(out, os.path.join(target, f"game-{slot}.out"))
    traces = sorted(glob.glob(os.path.join(SHOTS, "coop-*.jsonl")), key=os.path.getmtime)
    if traces:
        shutil.copy2(traces[-1], target)
    print(f"rig: {len(copied)} log(s) and "
          f"{'the newest trace' if traces else 'no trace'} in {target}")
    return 0


# Read-only game state through $RIG/tools-tmp/peek.exe (ReadProcessMemory, no
# debugger). Addresses: docs/FINDINGS.md ("Host visitor allowance",
# "Rig artifact: second-visitor load stall") and docs/RE_TOKOYO_COOP.md.
STATE_FIELDS = (
    ("loader", 4, "*0x1418715e0+0x5d0"),     # mission loader phase (12 = loaded)
    ("tokoyo", 1, "*0x1418715e0+0x660"),     # Tokoyo Coop (companion) mode
    ("role", 4, "*0x1418715e0+0x5b0"),       # 0 host, 1 visitor
    ("mode", 4, "0x14191de34"),              # 1 companion session, 0 shrine visitor
    ("state", 4, "0x141790e30"),             # session root +0x3c0
    ("own", 1, "0x141790e38"),               # own member slot (root +0x3c8)
    ("allow", 2, "0x141790e0c"),             # visitor allowances (root +0x39c/+0x39d)
    ("wait", 1, "*0x14189f408+0x15837c"),    # IsWaitCoopPlayer counters
    ("waitn", 1, "*0x14189f408+0x158388"),
)


def _frame_luma(slot):
    path = shot(slot, f"state-{slot}.png", settle=0.0) if resolve_slot(slot) else None
    if not path:
        return "-"
    out = subprocess.run(["magick", path, "-alpha", "off", "-format", "%[fx:mean]", "info:"],
                         capture_output=True, text=True)
    return f"{float(out.stdout or 0):.3f}"


def cmd_state(args):
    peek_args = []
    for _, size, expr in STATE_FIELDS:
        peek_args += [f"{size:x}", expr]
    for slot in args.slots:
        if not game_pids(slot):
            print(f"slot {slot}: not running")
            continue
        result = subprocess.run([RUN_SH, slot, "probe", "peek.exe", *peek_args],
                                capture_output=True, text=True, timeout=60,
                                env=dict(os.environ, PROTON_LOG="0"))
        values = []
        for line in result.stdout.splitlines():
            if "] @" in line and ":" in line.split("] @", 1)[1]:
                raw = line.split("] @", 1)[1].split(":", 1)[1].split()
                values.append(int.from_bytes(bytes(int(b, 16) for b in raw if b != "??"),
                                             "little") if "??" not in raw else None)
        fields = " ".join(f"{name}={value}" for (name, _, _), value
                          in zip(STATE_FIELDS, values))
        print(f"slot {slot}: {fields or 'peek failed'} frame={_frame_luma(slot)}")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Rig lifecycle.")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    up = sub.add_parser("up")
    up.add_argument("slots", nargs="+", choices=SLOTS)
    up.add_argument("--client", action="store_true",
                    help="start the slot b Steam client first")
    up.add_argument("--timeout", type=float, default=240.0,
                    help="seconds to wait for each slot's first frame")
    down = sub.add_parser("down")
    down.add_argument("slots", nargs="*", choices=SLOTS)
    down.add_argument("--client", action="store_true", help="also stop the Steam client")
    deploy = sub.add_parser("deploy")
    deploy.add_argument("build", choices=sorted(BUILDS))
    deploy.add_argument("slots", nargs="+", choices=SLOTS)
    ini = sub.add_parser("ini")
    ini.add_argument("slot", choices=SLOTS)
    ini.add_argument("pairs", nargs="+", metavar="KEY=VALUE")
    collect = sub.add_parser("collect")
    collect.add_argument("name")
    state = sub.add_parser("state")
    state.add_argument("slots", nargs="+", choices=SLOTS)
    args = parser.parse_args()
    return {"status": cmd_status, "up": cmd_up, "down": cmd_down, "deploy": cmd_deploy,
            "ini": cmd_ini, "collect": cmd_collect, "state": cmd_state}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
