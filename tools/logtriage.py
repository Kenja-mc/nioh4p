#!/usr/bin/env python3
"""logtriage.py - read nioh3p proxy runtime logs at a glance.

Stdlib only, read-only, deterministic. Reads the log the proxy writes
(``[HH:MM:SS.mmm] message`` lines) and prints a grouped timeline, decodes
lobby-data blobs, or follows a live session.

Usage
-----
  tools/logtriage.py                          # grouped timeline of the live log
  tools/logtriage.py LOG                      # ... of another log
  tools/logtriage.py LOG --sections data,mm   # only some sections
  tools/logtriage.py LOG --decode             # key + base64 blob decode report
  tools/logtriage.py LOG --watch              # tail -f, interesting lines only
  tools/logtriage.py LOG --grep ktol_invite   # substring or regex filter
  tools/logtriage.py LOG --since 17:41:00 --until 17:42:00

Sections: session hooks iface callbacks mm site eos friends p2p data other

Modes: --watch wins over --decode; both honour --grep/--since/--until.

Notes
-----
* A lobby-data value can contain raw bytes, including newlines (see
  ``ktol_common_key``): such a value continues on the following lines, which
  carry no timestamp. Those lines are attached to the line they belong to and
  printed with \\xNN escapes.
* The last line is marked ``[partial]`` when the file does not end in a
  newline (a half-written line caught mid-flush).
"""

from __future__ import annotations

import argparse
import base64
import binascii
import os
import re
import signal
import sys
import time

DEFAULT_LOG = os.path.expanduser("~/.cache/nioh3p/out/nioh3p.log")
MIN_WATCH_INTERVAL = 0.05         # floor for --watch-interval (0 polled as fast as possible)
WATCH_TAIL_READ = 262144          # bytes of history scanned for --watch --tail

SECTION_ORDER = [
    "session", "hooks", "iface", "callbacks", "mm", "site",
    "eos", "friends", "p2p", "data", "other",
]

SECTION_TITLES = {
    "session": "proxy lifecycle / config / dumps",
    "hooks": "hooks, resolved exports, thunk + vtable installs",
    "iface": "steam interface fetches",
    "callbacks": "callback registrations + Run invocations",
    "mm": "matchmaking calls",
    "site": "game call sites (proxy site: reports)",
    "eos": "EOS lobby calls",
    "friends": "friends calls",
    "p2p": "p2p / socket calls",
    "data": "lobby data (ktol_* / binary_* / integer_*)",
    "other": "unclassified lines",
}

# Categories kept while following a live session (--watch). session/hooks are
# included so a crash or a failed hook install is visible live, not only in the
# timeline pass.
WATCH_SECTIONS = ("session", "hooks", "iface", "callbacks", "mm", "site", "eos",
                  "friends", "p2p", "data")

MM_PREFIXES = (
    "Steam_ISteamMatchmaking", "Steam matchmaking interface", "Steam CreateLobby",
    "Steam SetLobby", "Steam GetLobby", "Steam GetNumLobbyMembers", "Steam JoinLobby",
    "Steam RequestLobbyData", "Steam LeaveLobby", "Steam SendLobbyChatMsg",
    "Steam InviteUserToLobby",
)

# ------------------------------------------------------------ byte/text views


def vis(text: str) -> str:
    """Make text safe for stdout: escape surrogates, controls and newlines."""
    out = []
    for ch in text:
        code = ord(ch)
        if 0xDC80 <= code <= 0xDCFF:          # surrogateescape byte
            out.append("\\x%02x" % (code - 0xDC00))
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif code < 0x20 or code == 0x7F:
            out.append("\\x%02x" % code)
        else:
            out.append(ch)
    return "".join(out)


def raw_bytes(text: str) -> bytes:
    """Recover the original bytes of text decoded with surrogateescape."""
    return text.encode("utf-8", "surrogateescape")


def ascii_view(data: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data)


def hex_field(data: bytes, per: int = 8) -> str:
    """'86 68 07 00 00 00 00 00  01 00 ...' with a gap every `per` bytes."""
    parts = []
    for i, b in enumerate(data):
        parts.append("%02x" % b)
        if per and (i + 1) % per == 0 and i + 1 != len(data):
            parts.append("")
    return " ".join(parts)


def hexdump_lines(data: bytes, limit: int, width: int = 16, per: int = 8) -> list:
    """Readable hexdump rows: offset, hex, ascii."""
    pad = width * 3 + (width // per - 1)
    rows = []
    for off in range(0, min(len(data), limit), width):
        row = data[off:off + width]
        hx = hex_field(row, per)
        rows.append("    %04x  %s%s  |%s|" % (off, hx, " " * max(0, pad - len(hx)),
                                              ascii_view(row)))
    return rows


def u32_view(data: bytes, count: int = 8) -> str:
    n = min(count, len(data) // 4)
    if n <= 0:
        return ""
    vals = []
    for i in range(n):
        v = int.from_bytes(data[i * 4:i * 4 + 4], "little")
        vals.append("%u/0x%08x" % (v, v))
    return "u32[0..%d] = %s" % (n - 1, " ".join(vals))


def u64_view(data: bytes, count: int = 4) -> str:
    n = min(count, len(data) // 8)
    if n <= 0:
        return ""
    vals = []
    for i in range(n):
        v = int.from_bytes(data[i * 8:i * 8 + 8], "little")
        vals.append("%u/0x%016x" % (v, v))
    return "u64[0..%d] = %s" % (n - 1, " ".join(vals))


def strings_in(data: bytes, minlen: int = 4, cap: int = 8) -> list:
    """Printable ASCII runs as (offset, text)."""
    out = []
    start = -1
    for i, b in enumerate(data):
        if 32 <= b < 127:
            if start < 0:
                start = i
        elif start >= 0:
            if i - start >= minlen:
                out.append((start, data[start:i].decode("ascii")))
            start = -1
    if start >= 0 and len(data) - start >= minlen:
        out.append((start, data[start:].decode("ascii")))
    return out[:cap]


def try_b64(text: str):
    """Decoded bytes if `text` is a plausible base64 blob, else None."""
    if len(text) < 4 or len(text) % 4 != 0:
        return None
    if not re.fullmatch(r"[A-Za-z0-9+/]+={0,2}", text):
        return None
    try:
        return base64.b64decode(text, validate=True)
    except (binascii.Error, ValueError):
        return None


def clip(text: str, width: int) -> str:
    text = vis(text)
    if width and len(text) > width:
        return text[: max(1, width - 1)] + "\u2026"
    return text


# ------------------------------------------------------------- line pattern

TS_RE = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]\s?(.*)$", re.S)
TS_ARG_RE = re.compile(r"^(\d{1,2}):(\d{2})(?::(\d{2})(?:\.(\d{1,3}))?)?$")
KV_PAIR_RE = re.compile(r'key="([^"]*)"\s+value="([^"]*)"\s*(?:->\s*(\S+))?\s*$')
KV_ARROW_RE = re.compile(r'key="([^"]*)"\s*->\s*"([^"]*)"\s*$')
EOS_ATTR_RE = re.compile(r'key="([^"]*)"\s+(bool|int64|double|string)=(?:"([^"]*)"|(\S+))')
PAYLOAD_RE = re.compile(r"^steam cb: payload:\s*((?:[0-9a-fA-F]{2}[ \t]*)+)$")
IFACE_RE = re.compile(r'Steam FindOrCreateUserInterface\("([^"]*)"\) -> (\S+)$')
RUN_RE = re.compile(r"steam cb: Run id=(-?\d+) obj=(\S+)(?: class (.+?))? param=(\S+)"
                    r"(?: ioFailure=\d+ call=\d+)?$")
REG_RE = re.compile(r"steam cb: (RegisterCallback|RegisterCallResult) id=(-?\d+) obj=(\S+) "
                    r"vtbl=(\S+) run=(\S+)(?: class (.+))?$")
REG_AT_RE = re.compile(r"steam cb: Run at (\S+) watched \(cb (-?\d+)\)")


def categorize(msg: str) -> str:
    if msg.startswith("steam cb:"):
        return "callbacks"
    if msg.startswith("Steam friends"):
        return "friends"
    if msg.startswith("Steam p2p:") or msg.startswith("Steam sockets:"):
        return "p2p"
    if msg.startswith("Steam FindOrCreateUserInterface"):
        return "iface"
    if msg.startswith(MM_PREFIXES):
        return "mm"
    if msg.startswith(("site:", "sites:")):
        return "site"
    if msg.startswith("EOS_"):
        return "eos"
    if msg.startswith(("hook:", "eos:", "steam: ", "DirectInput8Create")):
        return "hooks"
    if msg.startswith(("=== nioh3p", "config:", "output:", "worker:", "dump:",
                       "decrypt-probe:", "backtrace:")):
        return "session"
    return "other"


def data_op(msg: str):
    if msg.startswith("Steam SetLobbyData"):
        return "set"
    if msg.startswith("Steam SetLobbyMemberData"):
        return "member-set"
    if msg.startswith("Steam GetLobbyDataByIndex"):
        return "index-get"
    if msg.startswith("Steam GetLobbyData"):
        return "get"
    if msg.startswith("Steam GetLobbyMemberData"):
        return "member-get"
    return None


def extract_pairs(msg: str) -> list:
    """[(op, key, value)] for every key/value pair on the (logical) line."""
    op = data_op(msg)
    if op:
        m = KV_PAIR_RE.search(msg)
        if m:
            return [(op, m.group(1), m.group(2))]
        m = KV_ARROW_RE.search(msg)
        if m:
            return [(op, m.group(1), m.group(2))]
        return []
    if msg.startswith("EOS_LobbyModification_AddAttribute"):
        m = EOS_ATTR_RE.search(msg)
        if m:
            return [("eos-attr", m.group(1), m.group(3) if m.group(3) is not None else m.group(4))]
    return []


class Rec:
    """One logical log line (a physical line plus any continuation lines)."""

    __slots__ = ("ts", "tstr", "msg", "full", "cont", "cat", "partial",
                 "payload", "pairs")

    def __init__(self, ts, tstr, msg, partial):
        self.ts = ts
        self.tstr = tstr
        self.msg = msg
        self.full = msg
        self.cont = 0
        self.partial = partial
        self.payload = None
        self.cat = categorize(msg)
        self.pairs = extract_pairs(msg)
        self._payload()

    def add_cont(self, text):
        self.cont += 1
        self.full += "\n" + text
        self.pairs = extract_pairs(self.full)
        self._payload()

    def _payload(self):
        m = PAYLOAD_RE.match(self.full)
        if m:
            self.payload = bytes.fromhex(re.sub(r"[ \t]+", "", m.group(1)))
            self.cat = "callbacks"


def decode_line(raw: bytes) -> str:
    text = raw.decode("utf-8", "surrogateescape")
    if text.endswith("\n"):
        text = text[:-1]
    if text.endswith("\r"):
        text = text[:-1]
    return text


def make_rec(text: str, partial: bool) -> Rec:
    m = TS_RE.match(text)
    if not m:
        return Rec(None, None, text, partial)
    hh, mm, ss, ms = (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
    ts = ((hh * 60 + mm) * 60 + ss) * 1000 + ms
    return Rec(ts, "%02d:%02d:%02d.%03d" % (hh, mm, ss, ms), m.group(5), partial)


def load_records(path):
    """[(Rec, ...)], truncated -> True when the last line has no newline."""
    recs = []
    truncated = False
    with open(path, "rb") as fh:
        prev = None
        for raw in fh:
            if prev is not None:
                add_line(recs, prev, False)
            prev = raw
        if prev is not None:
            truncated = not prev.endswith(b"\n")
            add_line(recs, prev, truncated)
    return recs, truncated


def add_line(recs, raw: bytes, partial: bool):
    text = decode_line(raw)
    m = TS_RE.match(text)
    if m:
        recs.append(make_rec(text, partial))
        return
    if recs and recs[-1].ts is not None:      # continuation of a binary value
        recs[-1].add_cont(text)
        if partial:
            recs[-1].partial = True
        return
    recs.append(Rec(None, None, text, partial))


# ---------------------------------------------------------------- filtering


def build_matcher(pattern):
    if not pattern:
        return None
    try:
        rx = re.compile(pattern)
    except re.error:
        rx = re.compile(re.escape(pattern))

    def match(text):
        return bool(rx.search(text))

    match.pattern = pattern
    return match


def parse_ts_arg(text, which):
    m = TS_ARG_RE.match(text.strip())
    if not m:
        raise SystemExit("logtriage: bad --%s value '%s' (want HH:MM[:SS[.mmm]])" % (which, text))
    hh, mm = int(m.group(1)), int(m.group(2))
    ss = int(m.group(3)) if m.group(3) else 0
    ms = int((m.group(4) or "0").ljust(3, "0"))
    if hh > 23 or mm > 59 or ss > 59:
        raise SystemExit("logtriage: bad --%s value '%s'" % (which, text))
    return ((hh * 60 + mm) * 60 + ss) * 1000 + ms


def fmt_ms(ms):
    return "%02d:%02d:%02d.%03d" % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60, ms % 1000)


def make_window(since, until):
    lo = parse_ts_arg(since, "since") if since else None
    hi = parse_ts_arg(until, "until") if until else None
    if lo is None and hi is None:
        return None
    return (lo, hi)


def in_window(ts, win):
    if win is None:
        return True
    if ts is None:
        return False
    lo, hi = win
    if lo is not None and hi is not None and hi < lo:      # window wraps midnight
        return ts >= lo or ts <= hi
    if lo is not None and ts < lo:
        return False
    if hi is not None and ts > hi:
        return False
    return True


def keep(rec, win, match, extra=""):
    if not in_window(rec.ts, win):
        return False
    if match and not match(rec.full + extra):
        return False
    return True


# ------------------------------------------------------------------ summary


def render_summary(path, recs, shown, truncated, win, match, note=None):
    out = ["== summary =="]
    times = [r.tstr for r in recs if r.ts is not None]
    out.append("file      : %s" % path)
    out.append("lines     : %d logical line(s), %d timestamped, %d continuation"
               % (len(recs), len(times), sum(r.cont for r in recs)))
    if times:
        out.append("span      : %s .. %s" % (times[0], times[-1]))
    loads = [r.tstr or "(no time)" for r in recs
             if r.msg.startswith("=== nioh3p proxy loading")]
    if loads:
        out.append("sessions  : %d proxy load(s): %s" % (len(loads), " ".join(loads)))
    out.append("partial   : %s" % ("YES - last line has no trailing newline (marked [partial])"
                                   if truncated else "no"))
    if win:
        out.append("window    : since %s until %s" % (
            fmt_ms(win[0]) if win[0] is not None else "start",
            fmt_ms(win[1]) if win[1] is not None else "end"))
    if match:
        out.append("grep      : %s" % match.pattern)
    counts = {}
    for r in recs:
        counts[r.cat] = counts.get(r.cat, 0) + 1
    out.append("lines/kind: %s" % (" ".join("%s=%d" % (k, counts[k])
                                            for k in SECTION_ORDER if counts.get(k)) or "-"))
    shown_txt = " ".join("%s=%d" % ("data-pairs" if k == "data" else k, shown[k])
                         for k in SECTION_ORDER if shown.get(k))
    if note:
        shown_txt = (shown_txt + " " if shown_txt else "") + note
    out.append("printed   : %s" % (shown_txt or "nothing"))
    out.append("")
    return out


# ------------------------------------------------------------- timeline view


def build_timeline(recs, args, win, match, sections):
    out = []
    shown = {}
    grouped = {c: [] for c in SECTION_ORDER}
    for r in recs:
        if r.cat in sections and keep(r, win, match):
            grouped[r.cat].append(r)

    for cat in SECTION_ORDER:
        if cat not in sections or cat == "data":
            continue
        rows = grouped[cat]
        if not rows and len(sections) > 1:
            continue
        out.append("== %s - %s (%d) ==" % (cat, SECTION_TITLES[cat], len(rows)))
        shown[cat] = len(rows)
        if cat == "callbacks":
            out.extend(callback_index(rows))
        for r in rows:
            out.extend(timeline_row(r, args))
        out.append("")

    if "data" in sections:
        rows, pairs = build_data_rows(recs, args, win, match)
        out.append("== data - %s (%d distinct value(s), %d pair occurrence(s)) =="
                   % (SECTION_TITLES["data"], len(rows), pairs))
        shown["data"] = pairs
        for key, val, ops, first, last, seen, note in rows:
            out.append("%-20s %-22s @%-12s %s  \"%s\""
                       % (key, ops, first, note, clip(val, max(24, args.width - 78))))
        if rows:
            out.append("")
        return out, shown
    return out, shown


SITE_RE = re.compile(r"^site:\s*(\S+)\s+from\s+(\S+)$")


def site_line(text: str) -> str:
    """'site: SteamCreateLobby from exe+0x1e0f' -> 'SteamCreateLobby <- exe+0x1e0f'."""
    m = SITE_RE.match(text)
    if not m:
        return text
    return "%-26s <- %s" % (m.group(1), m.group(2))


def callback_index(rows):
    """Compact 'which callback object maps to which Run, how often it fired'."""
    run_of = {}
    regs = {}
    runs = {}
    payloads = {}
    classes = {}
    last_cb = None
    for r in rows:
        if r.payload is not None:
            if last_cb is not None:
                payloads[last_cb] = payloads.get(last_cb, 0) + 1
            continue
        m = REG_AT_RE.match(r.full)
        if m:
            run_of[m.group(2)] = m.group(1)
            regs[m.group(2)] = regs.get(m.group(2), 0) + 1
            continue
        m = REG_RE.match(r.full)
        if m:
            if m.group(6):
                classes[m.group(2)] = m.group(6)
            continue
        m = RUN_RE.match(r.full)
        if m:
            last_cb = m.group(1)
            runs[last_cb] = runs.get(last_cb, 0) + 1
            if m.group(3):
                classes.setdefault(last_cb, m.group(3))
    out = []
    for cid in sorted(set(run_of) | set(runs), key=lambda c: (run_of.get(c) or "", c)):
        out.append("  cb %-11s run=%-12s registrations=%-2d Run-invocations=%-2d payloads=%-2d class=%s"
                   % (cid, run_of.get(cid, "?"), regs.get(cid, 0), runs.get(cid, 0),
                      payloads.get(cid, 0), vis(classes.get(cid, "-"))))
    return out


def timeline_row(r, args):
    tag = "[partial] " if r.partial else ""
    tstr = r.tstr or "(no time)"
    if r.payload is not None:
        pad = " " * 11
        out = ["%spayload %d B: %s" % (pad, len(r.payload), hex_field(r.payload))]
        out.append("%s  %s  ascii=%s" % (pad, u32_view(r.payload, 4), ascii_view(r.payload)))
        more = u64_view(r.payload, 2)
        if more:
            out.append("%s  %s" % (pad, more))
        return out
    if r.cat == "site" and not r.cont:
        return ["%s %s%s" % (tstr, tag, clip(site_line(r.full), args.width))]
    return ["%s %s%s" % (tstr, tag, clip(r.full, args.width))]


def build_data_rows(recs, args, win, match):
    """[(key, val, ops, first, last, seen, note)] sorted by first appearance."""
    groups = {}
    order = []
    total = 0
    for r in recs:
        for op, key, val in r.pairs:
            if not keep(r, win, match, key + " " + val):
                continue
            total += 1
            ident = (key, val)
            g = groups.get(ident)
            if g is None:
                g = {"key": key, "val": val, "ops": {}, "first": r.tstr,
                     "ts": r.ts, "last": r.tstr, "seen": 0}
                groups[ident] = g
                order.append(ident)
            g["ops"][op] = g["ops"].get(op, 0) + 1
            g["seen"] += 1
            if r.ts is not None and (g["ts"] is None or r.ts < g["ts"]):
                g["ts"], g["first"] = r.ts, r.tstr
            g["last"] = r.tstr
    rows = [groups[i] for i in order]
    rows.sort(key=lambda g: (g["ts"] if g["ts"] is not None else -1, g["key"], g["val"]))
    out = []
    for g in rows:
        blob = try_b64(g["val"])
        if blob is None:
            note = "raw %dB" % len(raw_bytes(g["val"]))
        elif not blob.strip(b"\x00"):
            note = "b64 %dB all-zero" % len(blob)
        else:
            note = "b64 %dB %s" % (len(blob), hex_field(blob[:4]))
        ops = " ".join("%s\u00d7%d" % (k, v) for k, v in sorted(g["ops"].items()))
        out.append((g["key"], g["val"], ops, g["first"], g["last"], g["seen"], note))
    return out, total


# --------------------------------------------------------------- decode view


def build_decode(recs, args, win, match):
    rows, total = build_data_rows(recs, args, win, match)
    b64_rows = [r for r in rows if try_b64(r[1]) is not None]
    raw_rows = [r for r in rows if try_b64(r[1]) is None]

    out = []
    out.append("== decoded lobby-data blobs (%d distinct base64 pair(s)) ==" % len(b64_rows))
    for key, val, ops, first, last, seen, _note in b64_rows:
        blob = try_b64(val)
        zero = not blob.strip(b"\x00")
        out.append("")
        out.append("%s  [%s]  %s  first %s  last %s  %d occurrence(s)"
                   % (key, ops, "ALL-ZERO" if zero else "non-zero", first, last, seen))
        out.append("  base64   : %s%s" % (clip(val, 72), "\u2026" if len(val) > 72 else ""))
        out.append("  decoded  : %d byte(s)%s" % (
            len(blob), ", ALL ZERO" if zero else ""))
        out.extend(hexdump_lines(blob, 64))
        out.append("  ascii    : %s" % ascii_view(blob[:64]))
        if zero:
            out.append("  note     : %d zero byte(s)%s"
                       % (len(blob), "" if len(blob) <= 64 else " (first 64 shown)"))
        u32 = u32_view(blob, 8)
        u64 = u64_view(blob, 4)
        if u32:
            out.append("  %s" % u32)
        if u64:
            out.append("  %s" % u64)
        strs = strings_in(blob)
        if strs:
            out.append("  strings  : %s" % "  ".join(
                "off=%d len=%d %r" % (off, len(s), s) for off, s in strs))
        tail = blob[64:96]
        if tail:
            out.append("  next32B  : %s" % hex_field(tail))
    if not b64_rows:
        out.append("(none)")

    if raw_rows:
        out.append("")
        out.append("== non-base64 lobby-data values (%d) ==" % len(raw_rows))
        for key, val, ops, first, _last, seen, _note in raw_rows:
            b = raw_bytes(val)
            out.append("")
            out.append("%s  [%s]  raw %d byte(s)  first %s  %d occurrence(s)"
                       % (key, ops, len(b), first, seen))
            if len(b) <= 16:
                out.append("  raw      : %s" % clip(val, 72))
                out.append("  hex      : %s" % hex_field(b, 0))
                out.append("  ascii    : %s" % ascii_view(b))
            else:
                out.append("  raw      : %s" % clip(val, 72))
                out.extend(hexdump_lines(b, 64))
                out.append("  ascii    : %s" % ascii_view(b[:64]))

    out.append("")
    out.append("total     : %d distinct key/value pair(s): %d base64, %d raw (%d occurrence(s))"
               % (len(rows), len(b64_rows), len(raw_rows), total))
    return out


# ------------------------------------------------------------------- --watch

WATCH_TAG = {
    "session": "sess ",
    "hooks": "hook ",
    "iface": "iface",
    "callbacks": "cb   ",
    "mm": "mm   ",
    "site": "site ",
    "eos": "eos  ",
    "friends": "frnd ",
    "p2p": "p2p  ",
    "data": "data ",
}



def watch_lines(rec, args):
    """One printable line per event, plus a data one-liner per lobby-data pair."""
    out = []
    msg = rec.full
    cat = rec.cat
    if cat == "iface":
        m = IFACE_RE.match(msg)
        body = "iface %s -> %s" % (m.group(1), m.group(2)) if m else msg
    elif cat == "callbacks":
        if rec.payload is not None:
            u32 = u32_view(rec.payload, 4)
            body = "payload %dB %s%s" % (len(rec.payload), hex_field(rec.payload),
                                         "  " + u32 if u32 else "")
        else:
            # keep every field the proxy printed (id/obj/vtbl/run/class/param)
            body = msg.replace("steam cb: ", "")
    elif cat == "site":
        body = site_line(msg)
    else:
        body = msg
    out.append("%s %s %s" % (rec.tstr or "(no time)", WATCH_TAG.get(cat, cat + " "),
                             clip(body, args.width)))
    for op, key, val in rec.pairs:
        blob = try_b64(val)
        if blob is None:
            note = "raw %dB" % len(raw_bytes(val))
        else:
            note = "b64 %dB %s" % (len(blob), hex_field(blob[:8]))
            if not blob.strip(b"\x00"):
                note += " (all zero)"
        out.append("%s data  %s %s = %s" % (rec.tstr or "(no time)", op, key, note))
    return out


def denied_error(path):
    return "cannot read %s: permission denied (check the file's mode)" % path


def read_error(path, exc):
    """'cannot read PATH: why' - shared by the one-shot and --watch paths."""
    if isinstance(exc, PermissionError):
        return denied_error(path)
    return "cannot read %s: %s" % (path, exc.strerror or exc)


def unreadable_state(path, exc):
    """(state key, notice line) for a log that cannot be read right now."""
    if isinstance(exc, FileNotFoundError):
        return "missing", "%s not present yet, waiting for it to appear" % path
    if isinstance(exc, PermissionError):
        return "denied", denied_error(path)
    return "io", read_error(path, exc)


def watch(path, args, win, match):
    secs = float(args.watch_seconds or 0)
    interval = float(args.watch_interval)
    counts = {}
    stopped = {"flag": False}

    def on_stop(_signum, _frame):
        stopped["flag"] = True

    signal.signal(signal.SIGINT, on_stop)
    try:
        signal.signal(signal.SIGTERM, on_stop)
    except (ValueError, OSError):
        pass

    print("--- logtriage --watch %s (interval %.2fs, tail %d%s) --- Ctrl-C to stop"
          % (path, interval, args.tail, ", auto-stop after %gs" % secs if secs else ""),
          flush=True)

    state = ["ok"]                    # last reported readability, see notice()

    def notice(kind, text):
        """Report a read failure/recovery once; unchanged polls stay silent."""
        if state[0] == kind:
            return
        state[0] = kind
        print("--- %s ---" % text, flush=True)

    def size_now():
        """Current size, or None (after reporting why) when it cannot be read."""
        try:
            size = os.path.getsize(path)
        except OSError as exc:
            notice(*unreadable_state(path, exc))
            return None
        if not os.access(path, os.R_OK):
            # stat() succeeds on an unreadable file, so without this the
            # failure would only surface once new data arrived -- with
            # --tail 0 that is never, which reads as "no events".
            notice("denied", denied_error(path))
            return None
        return size

    # Start at the end of the file: --tail 0 means "no history", and the --tail
    # scan below needs the current size anyway. Reading resumes at `pos`, so
    # replayed history never reaches the live event count.
    pos = size_now() or 0
    if args.tail > 0 and pos > 0:
        start = max(0, pos - WATCH_TAIL_READ)
        chunk = None
        try:
            with open(path, "rb") as fh:
                fh.seek(start)
                chunk = fh.read()
        except OSError as exc:
            notice(*unreadable_state(path, exc))
        if chunk:
            if start:
                nl = chunk.find(b"\n")
                chunk = chunk[nl + 1:] if nl >= 0 else b""
            lines = chunk.split(b"\n")
            if lines and lines[-1] == b"":
                lines.pop()
            picked = []
            for raw in reversed(lines):
                rec = make_rec(decode_line(raw), False)
                if rec.cat not in WATCH_SECTIONS or not keep(rec, win, match):
                    continue
                picked.append(rec)
                if len(picked) >= args.tail:
                    break
            picked.reverse()
            for rec in picked:
                for line in watch_lines(rec, args):
                    print(line, flush=True)
            if picked:
                print("--- (last %d interesting line(s) above; following new ones) ---"
                      % len(picked), flush=True)
    print("--- following ---", flush=True)

    deadline = time.time() + secs if secs else None
    pending = b""
    unknown = set()
    while not stopped["flag"]:
        if deadline is not None and time.time() >= deadline:
            break
        size = size_now()
        if size is None:               # not readable: noticed once by `notice`
            time.sleep(interval)
            continue
        if state[0] != "ok":
            notice("ok", "%s appeared" % path if state[0] == "missing"
                   else "%s is readable again" % path)
        if size < pos:                     # log rotated or truncated
            print("--- log truncated (size %d < offset %d), reading from the start ---"
                  % (size, pos), flush=True)
            pos = 0
            pending = b""
        if size > pos:
            try:
                with open(path, "rb") as fh:
                    fh.seek(pos)
                    chunk = fh.read(size - pos)
            except OSError as exc:
                notice(*unreadable_state(path, exc))
                time.sleep(interval)
                continue
            pos = size
            pending += chunk
            complete = pending.split(b"\n")
            pending = complete.pop()
            for raw in complete:
                rec = make_rec(decode_line(raw), False)
                if rec.cat not in WATCH_SECTIONS:
                    if rec.cat == "other" and not match:
                        pref = rec.msg.split(" ")[0][:40]
                        if pref not in unknown:
                            unknown.add(pref)
                            print("--- note: unclassified log kind (%s), not shown live: %s ---"
                                  % (pref, clip(rec.msg, 90)), flush=True)
                    continue
                if not keep(rec, win, match):
                    continue
                counts[rec.cat] = counts.get(rec.cat, 0) + 1
                for line in watch_lines(rec, args):
                    print(line, flush=True)
        time.sleep(interval)

    if pending.strip():
        print("--- half-written tail held back (%d byte(s)): %s ---"
              % (len(pending), clip(decode_line(pending), 80)), flush=True)
    total = sum(counts.values())
    print("--- stopped: %d event(s)%s ---" % (
        total, ": " + " ".join("%s=%d" % (k, counts[k])
                               for k in SECTION_ORDER if counts.get(k)) if counts else ""),
        flush=True)


# ---------------------------------------------------------------------- main


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="logtriage.py",
        description="Grouped timeline / decoder / follower for nioh3p proxy logs.",
        epilog="Sections: " + " ".join(SECTION_ORDER))
    ap.add_argument("log", nargs="?", default=DEFAULT_LOG,
                    help="log file (default: %s)" % DEFAULT_LOG)
    ap.add_argument("--decode", action="store_true",
                    help="decode every key/base64 value pair instead of the timeline")
    ap.add_argument("--watch", action="store_true",
                    help="follow the log (tail -f) printing only interesting lines")
    ap.add_argument("--grep", metavar="PATTERN",
                    help="keep only lines matching this regex (or plain substring)")
    ap.add_argument("--since", metavar="HH:MM:SS", help="drop lines before this time of day")
    ap.add_argument("--until", metavar="HH:MM:SS", help="drop lines after this time of day")
    ap.add_argument("--sections", metavar="LIST",
                    help="comma list of sections to print (default: everything but 'other')")
    ap.add_argument("--width", type=int, default=400,
                    help="max characters per message (default 400)")
    ap.add_argument("--tail", type=int, default=10,
                    help="--watch: interesting lines to show from the existing file (default 10)")
    ap.add_argument("--watch-interval", type=float, default=0.2,
                    help="--watch poll interval in seconds (default 0.2)")
    ap.add_argument("--watch-seconds", type=float, default=0.0,
                    help="--watch: stop automatically after N seconds (0 = forever)")
    args = ap.parse_args(argv)

    path = args.log
    if not args.watch and not os.path.exists(path):
        print("logtriage: no such file: %s" % path, file=sys.stderr)
        return 1

    match = build_matcher(args.grep)
    win = make_window(args.since, args.until)

    if args.watch:
        if not (args.watch_interval >= MIN_WATCH_INTERVAL):     # NaN fails this
            print("logtriage: --watch-interval %g is below the %.2fs floor, using %.2fs"
                  % (args.watch_interval, MIN_WATCH_INTERVAL, MIN_WATCH_INTERVAL),
                  file=sys.stderr)
            args.watch_interval = MIN_WATCH_INTERVAL
        watch(path, args, win, match)
        return 0

    try:
        recs, truncated = load_records(path)
    except OSError as exc:                 # same wording as the --watch path
        print("logtriage: " + read_error(path, exc), file=sys.stderr)
        return 1

    if args.decode:
        body = build_decode(recs, args, win, match)
        pairs = sum(len(r.pairs) for r in recs
                    if keep(r, win, match, " ".join(p[1] + " " + p[2] for p in r.pairs)))
        print("\n".join(render_summary(path, recs, {}, truncated, win, match,
                                       note="%d decoded pair(s)" % pairs)))
        print("\n".join(body))
        return 0

    if args.sections:
        sections = [s.strip() for s in args.sections.split(",") if s.strip()]
        bad = [s for s in sections if s not in SECTION_ORDER]
        if bad:
            raise SystemExit("logtriage: unknown section(s): %s (known: %s)"
                             % (", ".join(bad), " ".join(SECTION_ORDER)))
    else:
        sections = [s for s in SECTION_ORDER if s != "other"]

    body, shown = build_timeline(recs, args, win, match, sections)
    print("\n".join(render_summary(path, recs, shown, truncated, win, match)))
    print("\n".join(body))
    hidden = [r for r in recs if r.cat not in sections and r.partial]
    skipped = [r for r in recs if r.cat not in sections]
    if skipped:
        kinds = " ".join(sorted({r.cat for r in skipped}))
        print("(%d line(s) hidden by section selection: %s - add them with --sections)"
              % (len(skipped), kinds))
    if hidden:
        print("(%d hidden line(s) are marked [partial])" % len(hidden))
    return 0


if __name__ == "__main__":
    sys.exit(main())
