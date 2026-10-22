#!/usr/bin/env python3
"""Decision-model driver for the headless Nioh co-op rig.

A screenshot goes to a vision decision model, which answers the screen, the
highlighted menu item and two predicates; a deterministic per-state table then
sends only the keys of a verified recipe. No OCR, no guessing a transition.

Two backends:
  mercury  OpenRouter Mercury Decide (free, default): posts to
           https://openrouter.ai/api/alpha/decisions with choice criteria and
           noul predicates. The OpenRouter free tier is rate limited.
  luna     Experiential Labs Decisions (gpt-6-luna-decisions).

usage:
  coop.py host b                       host offers co-op at the starting shrine
  coop.py visit a [--relieve c]        visitor joins through Torii Gate
  coop.py run --host b --visitors a c  host offer, then A, then C (A paused)
  --dry-run                            perceive once and print the action only
  --timeout SECONDS                    wall clock limit (default 600)
  --backend {mercury,luna}             default mercury, or $COOP_BACKEND
  --model MODEL                        override the backend's model id

Environment: OPENROUTER_API_KEY, or OPENROUTER_ENV_FILE naming a file that
holds an `OPENROUTER_API_KEY=` line (mercury); EXPLABS_API_KEY or
EXPLABS_KEY_FILE (luna). Keys are never printed. A trace line is appended to
$RIG/shots/coop-<timestamp>.jsonl for every step.
"""
import argparse
import base64
import json
import os
import signal
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nioh_nav import RIG, SHOTS, SLOTS, Input, resolve_slot, shot  # noqa: E402

OPENROUTER_URL = "https://openrouter.ai/api/alpha/decisions"
LUNA_URL = "https://api.experientiallabs.ai/v1/decisions"
MERCURY_MODEL = "inception/mercury-decide:free"
LUNA_MODEL = "gpt-6-luna-decisions"
BACKENDS = ("mercury", "luna")
DEFAULT_BACKEND = "mercury"
BACKEND_ENV = "COOP_BACKEND"
STEP_LIMIT = 60
CONF_MIN = 0.60
NODE_CONF_MIN = 0.90
PRED_MIN = 0.50
HOST_WALK_LONG_MS = 300
HOST_WALK_SHORT_MS = 150
HOST_WALK_STUCK = 4
HOST_SUMMON_RETURNS = 2
RETURN_UNCHANGED = 3
RETURN_SETTLE = 1.5
SENT_KEY_LIMIT = 6
LOADING = {"searching", "notice", "other"}
OTHER_LIMIT = 15
UNCERTAIN = "uncertain"
HOST_NEEDLE = "Steam_ISteamMatchmaking::CreateLobby"
VISIT_NEEDLE = "Steam JoinLobby:"

SCREEN_CHOICES = [
    ("title", "Title screen menu: NEW GAME / CONTINUE / LOAD GAME / SYSTEM."),
    ("notice", "Boot notice, logo, \"press any button\", or a black/loading screen "
               "with only a spinner."),
    ("lore", "Full-screen artwork with a paragraph of tip/lore text and a CONTINUE "
             "prompt bottom right."),
    ("world_map", "The world map with region name top-left; no menu panel open."),
    ("map_menu", "World map with a left menu panel: Shrine, Blacksmith, Dojo, "
                 "Torii Gate, Hidden Teahouse, Storehouse."),
    ("torii_menu", "Torii Gate menu: Yokai Realm with a Companion / Random Encounters."),
    ("become_visitor", "\"Become a Visitor\" panel with With Conditions, Secret Word "
                       "and a Search button."),
    ("searching", "Searching for a co-op partner / matchmaking wait dialog."),
    ("shrine_menu", "Shrine menu of wooden ema plaques (Make Offering, Level Up, "
                    "Summon Visitor, Boons...)."),
    ("summon_status", "Summon Visitor status panel: 'Invoke Visitor (will remain "
                      "active until you stop the offering)' with STOP OFFERING / "
                      "BACK prompts."),
    ("dialog", "A yes/no or confirmation dialog box over the game."),
    ("in_mission", "Third-person gameplay in a mission with health bars top-left, "
                   "no menu open."),
    ("other", "Anything else."),
]
HIGHLIGHT_CHOICES = [
    ("continue", "CONTINUE on the title menu"),
    ("new_game", "Title menu: NEW GAME"),
    ("load_or_system", "Title menu: LOAD GAME or SYSTEM"),
    ("starting_point_node", "World map: the selected node is the shrine-like STARTING "
                            "POINT (label STARTING POINT shown above the highlighted "
                            "node)"),
    ("other_map_node", "World map: a mission or other node is selected (a mission "
                       "info panel on the right, or a label other than STARTING POINT)"),
    ("shrine_item", "Map menu: Shrine"),
    ("torii_gate", "Map menu: Torii Gate"),
    ("other_map_menu_item", "Map menu: Blacksmith, Dojo, Hidden Teahouse or Storehouse"),
    ("random_encounters", "Torii menu: Random Encounters"),
    ("yokai_realm_companion", "Torii menu: Yokai Realm with a Companion"),
    ("search_button", "Become a Visitor: the Search button"),
    ("other_visitor_field", "Become a Visitor: With Conditions or Secret Word row"),
    ("summon_visitor", "Shrine ema plaques: SUMMON VISITOR plaque enlarged in front"),
    ("other_plaque", "Shrine ema plaques: another plaque in front"),
    ("confirm_button", "Information dialog with a single CONFIRM/OK button highlighted"),
    ("yes", "Yes/No dialog: YES highlighted"),
    ("no", "Yes/No dialog: NO highlighted"),
    ("none", "No menu cursor visible"),
]
SCREEN_VALUES = {value for value, _ in SCREEN_CHOICES}
HIGHLIGHT_VALUES = {value for value, _ in HIGHLIGHT_CHOICES}
PROMPT = ("Screenshot from a Nioh test rig, 1280x720. Answer every question about "
          "this frame only.")
SCREEN_INSTRUCTIONS = "Classify the current Nioh screen with exactly one choice."
HIGHLIGHT_INSTRUCTIONS = ("Which menu item or map node is currently highlighted/"
                          "selected by the cursor?")
PRAY_INSTRUCTIONS = "Is a PRAY button prompt visible near the shrine?"
COOP_INSTRUCTIONS = ("Is another player character (gold-outlined visitor, or a name "
                     "plate like Nioh-LAN-A/B/C) visible or listed in the party HUD "
                     "at top-left?")
QUESTIONS = [
    {"type": "choice", "name": "screen", "instructions": SCREEN_INSTRUCTIONS,
     "choices": [{"value": v, "description": d} for v, d in SCREEN_CHOICES]},
    {"type": "choice", "name": "highlight", "instructions": HIGHLIGHT_INSTRUCTIONS,
     "choices": [{"value": v, "description": d} for v, d in HIGHLIGHT_CHOICES]},
    {"type": "predicate", "name": "pray_prompt", "instructions": PRAY_INSTRUCTIONS},
    {"type": "predicate", "name": "coop_partner", "instructions": COOP_INSTRUCTIONS},
]
MERCURY_QUESTIONS = {
    "screen": {"type": "choice", "instructions": SCREEN_INSTRUCTIONS,
               "criteria": dict(SCREEN_CHOICES)},
    "highlight": {"type": "choice", "instructions": HIGHLIGHT_INSTRUCTIONS,
                  "criteria": dict(HIGHLIGHT_CHOICES)},
    "pray_prompt": {"type": "noul", "instructions": PRAY_INSTRUCTIONS},
    "coop_partner": {"type": "noul", "instructions": COOP_INSTRUCTIONS},
}


def openrouter_key():
    key = os.environ.get("OPENROUTER_API_KEY", "").strip()
    if key:
        return key
    path = os.environ.get("OPENROUTER_ENV_FILE")
    if path:
        try:
            with open(os.path.expanduser(path), encoding="utf-8") as env_file:
                for line in env_file:
                    name, _, value = line.partition("=")
                    if name.strip().removeprefix("export ").strip() != "OPENROUTER_API_KEY":
                        continue
                    key = value.strip().strip("'\"")
                    if key:
                        return key
        except OSError as exc:
            raise RuntimeError(f"OPENROUTER_ENV_FILE unreadable: {exc}") from exc
    raise RuntimeError("set OPENROUTER_API_KEY or OPENROUTER_ENV_FILE")


def explabs_key():
    key = os.environ.get("EXPLABS_API_KEY", "").strip()
    if key:
        return key
    path = os.environ.get("EXPLABS_KEY_FILE")
    if path:
        try:
            with open(os.path.expanduser(path), encoding="utf-8") as key_file:
                key = key_file.read().strip()
        except OSError as exc:
            raise RuntimeError(f"EXPLABS_KEY_FILE unreadable: {exc}") from exc
        if key:
            return key
    raise RuntimeError("set EXPLABS_API_KEY or EXPLABS_KEY_FILE")


def api_key(backend):
    return openrouter_key() if backend == "mercury" else explabs_key()


def _post(url, body, key):
    data = json.dumps(body).encode()
    headers = {"Content-Type": "application/json", "Authorization": "Bearer " + key}
    last = None
    for attempt in range(3):
        request = urllib.request.Request(url, data=data, headers=headers,
                                         method="POST")
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as exc:
            detail = exc.read()[:400].decode(errors="replace")
            last = RuntimeError(f"decision API HTTP {exc.code}: {detail}")
            if exc.code < 500 or attempt == 2:
                raise last
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last = RuntimeError(f"decision API request failed: {exc}")
            if attempt == 2:
                raise last
        time.sleep(1.5)
    raise last


def _collect_answers(payload):
    """Find answer objects regardless of the API's envelope."""
    found = {}

    def walk(node):
        if isinstance(node, dict):
            if isinstance(node.get("name"), str) and node.get("type") in (
                    "choice", "predicate", "refusal"):
                found.setdefault(node["name"], node)
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(payload)
    return found


class Perceiver:
    """One decision request per step: screen, highlight, two predicates."""

    def __init__(self, slot, backend=DEFAULT_BACKEND, model=None):
        self.slot = slot
        self.backend = backend
        self.model = model or (MERCURY_MODEL if backend == "mercury" else LUNA_MODEL)
        self._key = api_key(backend)
        self.calls = 0
        self.usage = {"input_tokens": 0, "output_tokens": 0, "cost": 0.0}

    def perceive(self, step=0):
        path = self._shot(step, "a", 0.25)
        answers, latency, usage = self._ask(path)
        low = self._low(answers)
        if low:
            path = self._shot(step, "b", 1.0)
            answers, latency, usage = self._ask(path)
            low = self._low(answers)
        screen_conf = round(float(answers["screen"].get("confidence", 0)), 3)
        highlight_conf = round(float(answers["highlight"].get("confidence", 0)), 3)
        raw_screen = answers["screen"].get("choice")
        raw_highlight = answers["highlight"].get("choice")
        raw = {
            "screen": {"choice": raw_screen, "confidence": screen_conf},
            "highlight": {"choice": raw_highlight, "confidence": highlight_conf},
        }
        # A low-confidence choice must never stop the run. "other" only waits;
        # "uncertain" matches no Return rule, so only safe moves and waits fire.
        screen = "other" if "screen" in low else raw_screen
        highlight = UNCERTAIN if "highlight" in low else raw_highlight
        if screen not in SCREEN_VALUES:
            raise RuntimeError(f"unknown screen choice {screen!r}")
        if highlight not in HIGHLIGHT_VALUES and highlight != UNCERTAIN:
            raise RuntimeError(f"unknown highlight choice {highlight!r}")
        pray = round(float(answers["pray_prompt"].get("probability", 0)), 3)
        coop = round(float(answers["coop_partner"].get("probability", 0)), 3)
        return {
            "screen": screen,
            "highlight": highlight,
            "screen_confidence": screen_conf,
            "highlight_confidence": highlight_conf,
            "pray_prompt": pray,
            "coop_partner": coop,
            "answers": {
                "screen": {"choice": screen, "confidence": screen_conf},
                "highlight": {"choice": highlight, "confidence": highlight_conf},
                "pray_prompt": pray,
                "coop_partner": coop,
            },
            "raw": raw,
            "screenshot": path,
            "latency": latency,
            "backend": self.backend,
            "model": self.model,
            "usage": usage,
        }

    def _low(self, answers):
        return [name for name in ("screen", "highlight")
                if float(answers[name].get("confidence", 0)) < CONF_MIN]

    def _shot(self, step, tag, settle):
        name = (f"coop-{self.slot}-{time.strftime('%H%M%S')}-{step}-{tag}.png")
        path = shot(self.slot, name, settle)
        if not path:
            raise RuntimeError(f"screenshot failed for slot {self.slot}")
        return path

    def _ask(self, path):
        with open(path, "rb") as image:
            encoded = base64.b64encode(image.read()).decode()
        started = time.monotonic()
        if self.backend == "mercury":
            answers, usage = self._mercury(encoded)
        else:
            answers, usage = self._luna(encoded)
        latency = time.monotonic() - started
        self.calls += 1
        for name, value in usage.items():
            self.usage[name] = self.usage.get(name, 0) + value
        return answers, latency, usage

    def _mercury(self, encoded):
        body = {
            "model": self.model,
            "state": [PROMPT, {"type": "image_url",
                               "image_url": {
                                   "url": "data:image/png;base64," + encoded}}],
            "questions": MERCURY_QUESTIONS,
        }
        payload = _post(OPENROUTER_URL, body, self._key)
        raw = payload.get("answers")
        if not isinstance(raw, dict):
            raise RuntimeError("Mercury Decide response missing answers")
        answers = {}
        for name in ("screen", "highlight"):
            answer = raw.get(name)
            if not isinstance(answer, dict) or not isinstance(answer.get("choice"), str):
                raise RuntimeError(f"Mercury Decide response missing choice {name!r}")
            answers[name] = {
                "choice": answer["choice"],
                "confidence": float(answer.get("confidence") or 0.0),
            }
        for name in ("pray_prompt", "coop_partner"):
            answer = raw.get(name)
            if not isinstance(answer, dict) or "noul" not in answer:
                raise RuntimeError(f"Mercury Decide response missing noul {name!r}")
            answers[name] = {"probability": float(answer["noul"])}
        return answers, self._usage(payload)

    def _luna(self, encoded):
        body = {
            "model": self.model,
            "input": [{"role": "user", "content": [
                {"type": "input_text", "text": PROMPT},
                {"type": "input_image",
                 "image_url": "data:image/png;base64," + encoded},
            ]}],
            "questions": QUESTIONS,
        }
        payload = _post(LUNA_URL, body, self._key)
        found = _collect_answers(payload)
        answers = {}
        for name in ("screen", "highlight", "pray_prompt", "coop_partner"):
            answer = found.get(name)
            if answer is None:
                raise RuntimeError(f"Decisions response missing answer {name!r}")
            if answer.get("type") == "refusal":
                raise RuntimeError(f"model refused question {name!r}")
            if name in ("screen", "highlight"):
                answers[name] = {
                    "choice": answer.get("choice"),
                    "confidence": float(answer.get("confidence") or 0.0),
                }
            else:
                answers[name] = {"probability": float(answer.get("probability", 0.0))}
        return answers, self._usage(payload)

    @staticmethod
    def _usage(payload):
        usage = payload.get("usage") or {}
        return {
            "input_tokens": int(usage.get("input_tokens") or 0),
            "output_tokens": int(usage.get("output_tokens") or 0),
            "cost": float(usage.get("cost") or 0.0),
        }


class LogOracle:
    """Only bytes written after construction count as new evidence."""

    def __init__(self, slot):
        self.path = os.path.join(RIG, f"lan-{slot}", "nioh3p.log")
        try:
            self.pos = os.path.getsize(self.path)
        except OSError:
            self.pos = 0
        self.text = ""

    def scan(self):
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return ""
        if size < self.pos:
            self.pos = 0
        if size == self.pos:
            return ""
        try:
            with open(self.path, "rb") as log:
                log.seek(self.pos)
                chunk = log.read(size - self.pos)
        except OSError:
            return ""
        self.pos += len(chunk)
        new = chunk.decode(errors="replace")
        self.text += new
        return new

    def found(self, needle):
        return needle in self.text


class Trace:
    def __init__(self):
        os.makedirs(SHOTS, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        path = os.path.join(SHOTS, f"coop-{stamp}.jsonl")
        suffix = 1
        while os.path.exists(path):
            path = os.path.join(SHOTS, f"coop-{stamp}-{suffix}.jsonl")
            suffix += 1
        self.path = path
        self.n = 0
        self._file = open(path, "w", encoding="utf-8")

    def write(self, **record):
        self.n += 1
        record["step"] = self.n
        self._file.write(json.dumps(record, sort_keys=True) + "\n")
        self._file.flush()

    def close(self):
        self._file.close()


def tap(key, ms=90, settle=0.8):
    return {"kind": "tap", "key": key, "ms": ms, "settle": settle}


def hold(key, seconds=1.7, settle=1.5):
    return {"kind": "hold", "key": key, "seconds": seconds, "settle": settle}


def wait(seconds=4.0):
    return {"kind": "wait", "seconds": seconds, "settle": seconds}


def rule(screen, action, when=None):
    return {"screen": screen, "when": when, "action": action}


HOST_TABLE = [
    # Once the lobby exists the offer is active: never Return again. Exit any
    # menu with BackSpace and only report success from gameplay.
    rule(None, tap("BackSpace", 90, 0.6),
         lambda d: d["host_offering"] and d["screen"] != "in_mission"),
    rule("notice", tap("Return", settle=1.5)),
    rule("title", tap("Return", settle=1.5),
         lambda d: (d["highlight"] == "continue"
                    and d["highlight_confidence"] >= NODE_CONF_MIN)),
    rule("title", tap("Down", 90, 0.6),
         lambda d: d["highlight"] == "new_game"),
    rule("title", tap("Up", 90, 0.6),
         lambda d: d["highlight"] == "load_or_system"),
    rule("title", wait(1.5)),
    rule("lore", tap("Return", settle=1.5)),
    rule("in_mission", hold("e", 1.7, 1.5),
         lambda d: d["pray_prompt"] >= PRED_MIN),
    rule("summon_status", tap("BackSpace", 90, 0.6)),
    rule("shrine_menu", tap("Return", settle=1.2),
         lambda d: (d["highlight"] == "summon_visitor"
                    and d["summon_returns"] < HOST_SUMMON_RETURNS)),
    rule("shrine_menu", tap("BackSpace", 90, 0.6),
         lambda d: d["highlight"] == "summon_visitor"),
    rule("shrine_menu", tap("Up", 90, 0.6)),
    rule("dialog", tap("Return", settle=1.2)),
    rule("other", wait(3.0)),
]

VISIT_TABLE = [
    # Once JoinLobby exists the join is in flight: send no keys at all, because
    # the world map reappears during loading and Return there is unsafe.
    rule(None, wait(3.0), lambda d: d["join_seen"]),
    rule("notice", tap("Return", settle=1.5)),
    rule("title", tap("Return", settle=1.5),
         lambda d: (d["highlight"] == "continue"
                    and d["highlight_confidence"] >= NODE_CONF_MIN)),
    rule("title", tap("Down", 90, 0.6),
         lambda d: d["highlight"] == "new_game"),
    rule("title", tap("Up", 90, 0.6),
         lambda d: d["highlight"] == "load_or_system"),
    rule("title", wait(1.5)),
    rule("lore", tap("Return", settle=1.5)),
    # DANGER: Return on any other world-map node starts a solo mission.
    rule("world_map", tap("Return", settle=1.5),
         lambda d: (d["highlight"] == "starting_point_node"
                    and d["highlight_confidence"] >= NODE_CONF_MIN)),
    # The map footer shows "B: STARTING POINT"; BackSpace jumps the cursor there
    # instead of cycling node by node.
    rule("world_map", tap("BackSpace", 90, 0.6)),
    rule("map_menu", tap("Return", settle=1.2),
         lambda d: d["highlight"] == "torii_gate"),
    rule("map_menu", tap("Down", 90, 0.6),
         lambda d: d["highlight"] in ("shrine_item", "other_map_menu_item")),
    rule("map_menu", tap("Down", 90, 0.6)),
    rule("torii_menu", tap("Return", settle=1.2),
         lambda d: d["highlight"] == "random_encounters"),
    rule("torii_menu", tap("Down", 90, 0.6)),
    rule("become_visitor", tap("Return", settle=1.2),
         lambda d: d["highlight"] == "search_button"),
    rule("become_visitor", tap("Down", 90, 0.6)),
    # A visitor never needs a yes/no dialog: confirm only a lone CONFIRM/OK
    # button, otherwise cancel (a stray yes could start a solo mission).
    rule("dialog", tap("Return", settle=1.2),
         lambda d: d["highlight"] == "confirm_button"),
    rule("dialog", tap("BackSpace", 90, 0.6)),
    rule("searching", wait(4.0)),
    # Only reachable after the JoinLobby oracle fired; the danger case is
    # rejected by _visit_check before any action is chosen.
    rule("in_mission", wait(4.0)),
    rule("other", wait(3.0)),
]


def _host_walk(walk):
    """At most one 300 ms walk, then one 150 ms retry, then a clean failure."""
    if not walk["long"]:
        walk["long"] = True
        return tap("w", HOST_WALK_LONG_MS, 1.0), None
    walk["stuck"] += 1
    if walk["short"]:
        return None, (f"host shows in_mission with no PRAY prompt after a "
                      f"{HOST_WALK_LONG_MS} ms and a {HOST_WALK_SHORT_MS} ms walk; "
                      f"stopping instead of walking away from the shrine")
    if walk["stuck"] >= HOST_WALK_STUCK:
        walk["short"] = True
        return tap("w", HOST_WALK_SHORT_MS, 1.0), None
    return wait(1.0), None


def match(table, decision):
    for entry in table:
        if entry["screen"] is not None and entry["screen"] != decision["screen"]:
            continue
        if entry["when"] and not entry["when"](decision):
            continue
        return entry["action"]
    return None


def action_text(action):
    if action is None:
        return "none"
    if action["kind"] == "wait":
        return f"wait {action['seconds']:.1f}s"
    if action["kind"] == "hold":
        return f"hold {action['key']} {action['seconds']:.1f}s"
    return f"tap {action['key']} {action['ms']}ms"


def perform(controller, action):
    if action["kind"] == "tap":
        controller.tap(action["key"], action["ms"])
    elif action["kind"] == "hold":
        controller.hold(action["key"], action["seconds"])
    time.sleep(action["settle"])


def slot_pid(slot):
    home = f"HOME={os.path.join(RIG, f'home-{slot}')}".encode()
    for process in os.scandir("/proc"):
        if not process.name.isdigit():
            continue
        try:
            with open(f"{process.path}/comm") as comm:
                if comm.read().strip() != "nioh.exe":
                    continue
            with open(f"{process.path}/environ", "rb") as environ:
                entries = environ.read().split(b"\0")
        except OSError:
            continue
        if home in entries:
            return int(process.name)
    return None


def pause_cycle(slot, seconds=5.0, pause=2.0):
    pid = slot_pid(slot)
    if pid is None:
        print(f"coop: relief: no live nioh.exe for slot {slot}")
        return False
    try:
        os.kill(pid, signal.SIGSTOP)
    except OSError as exc:
        print(f"coop: relief: SIGSTOP {slot} ({pid}) failed: {exc}")
        return False
    try:
        time.sleep(seconds)
    finally:
        try:
            os.kill(pid, signal.SIGCONT)
        except OSError:
            pass
    time.sleep(pause)
    return True


def _host_check(decision, oracle):
    if not oracle.found(HOST_NEEDLE):
        return "wait", None
    if decision["screen"] == "in_mission":
        return "success", ("host offering active (new CreateLobby line) and back "
                           "in the mission")
    return "wait", None


def _visit_check(decision, oracle):
    joined = oracle.found(VISIT_NEEDLE)
    if decision["screen"] == "in_mission":
        if not joined:
            return "fail", ("visitor reached in_mission without a new 'Steam JoinLobby:' "
                            "line; possible solo mission, stopping (no auto recovery)")
        if decision["coop_partner"] >= PRED_MIN:
            return "success", "visitor in the host mission (JoinLobby + coop partner)"
        return "wait", None
    return "wait", None


def _drive(slot, table, trace, check, *, dry_run=False, timeout=600.0, relief=None,
           backend=DEFAULT_BACKEND, model=None, walk=None):
    if not resolve_slot(slot):
        return False, f"no live game for slot {slot}"
    oracle = LogOracle(slot)
    perceiver = Perceiver(slot, backend, model)
    controller = None if dry_run else Input(SLOTS[slot]["display"])
    deadline = time.monotonic() + timeout
    last_key = None
    sent_same = 0
    other_run = 0
    retn = {"key": None, "count": 0}
    if relief:
        print(f"coop: relief: pausing slot {relief} for 5 s after every step")
    for step in range(STEP_LIMIT):
        if time.monotonic() > deadline:
            return False, f"wall timeout after {timeout:.0f}s"
        try:
            decision = perceiver.perceive(step)
        except Exception as exc:
            return False, f"perception failed: {exc}"
        oracle.scan()
        other_run = other_run + 1 if decision["screen"] == "other" else 0
        decision["host_offering"] = oracle.found(HOST_NEEDLE)
        decision["join_seen"] = oracle.found(VISIT_NEEDLE)
        decision["summon_returns"] = 0 if walk is None else walk["summon_returns"]
        verdict, why = check(decision, oracle)
        action = match(table, decision)
        stop = None
        if (walk is not None and decision["screen"] == "in_mission"
                and decision["pray_prompt"] < PRED_MIN):
            action, stop = _host_walk(walk)
        # Return debounce: a dropped key can leave the frame unchanged, so do
        # not hammer Return; wait, and only resend after 3 unchanged frames.
        key = (decision["screen"], decision["highlight"])
        sent_return = (action is not None and action["kind"] == "tap"
                       and action["key"] == "Return")
        if retn["key"] == key:
            if sent_return:
                retn["count"] += 1
                if retn["count"] < RETURN_UNCHANGED:
                    action = wait(RETURN_SETTLE)
                    sent_return = False
        elif retn["key"] is not None:
            retn["key"] = None
            retn["count"] = 0
        if key != last_key:
            sent_same = 0
        sent_key = (action is not None and action["kind"] in ("tap", "hold")
                    and decision["screen"] not in LOADING
                    and not decision["join_seen"])
        text = action_text(action)
        trace.write(slot=slot, screenshot=decision["screenshot"],
                    screen=decision["screen"], highlight=decision["highlight"],
                    answers=decision["answers"], raw=decision["raw"],
                    screen_confidence=decision["screen_confidence"],
                    highlight_confidence=decision["highlight_confidence"],
                    pray_prompt=round(decision["pray_prompt"], 3),
                    coop_partner=round(decision["coop_partner"], 3),
                    action=text, latency=round(decision["latency"], 2),
                    backend=decision["backend"], model=decision["model"],
                    usage=decision["usage"], dry_run=dry_run,
                    host_offering=decision["host_offering"],
                    summon_returns=decision["summon_returns"])
        print(f"coop step {trace.n} slot={slot} screen={decision['screen']} "
              f"highlight={decision['highlight']} action={text} "
              f"pray={decision['pray_prompt']:.2f} "
              f"coop={decision['coop_partner']:.2f} api={decision['latency']:.1f}s "
              f"backend={decision['backend']}")
        if verdict == "success":
            return True, why
        if verdict == "fail":
            return False, why
        if stop:
            return False, stop
        if dry_run:
            return True, f"dry-run: {text} in {decision['screen']}"
        if not decision["join_seen"] and other_run > OTHER_LIMIT:
            return False, (f"{other_run} consecutive unclassified 'other' frames "
                           f"(cap {OTHER_LIMIT}); no known screen is appearing")
        if action is None:
            return False, (f"unexpected state {decision['screen']}/"
                           f"{decision['highlight']} has no rule")
        if (walk is not None and decision["screen"] == "shrine_menu"
                and decision["highlight"] == "summon_visitor"
                and action["kind"] == "tap" and action["key"] == "Return"):
            walk["summon_returns"] += 1
        perform(controller, action)
        if sent_return:
            retn["key"] = key
            retn["count"] = 0
        if relief:
            pause_cycle(relief)
        if sent_key:
            sent_same += 1
            if sent_same >= SENT_KEY_LIMIT:
                return False, (f"no progress: {decision['screen']}/"
                               f"{decision['highlight']} after {sent_same} keys "
                               f"with no screen change (last action {text!r})")
        last_key = key
    return False, f"step limit {STEP_LIMIT} reached"


def host_offer(slot, trace=None, *, dry_run=False, timeout=600.0,
               backend=DEFAULT_BACKEND, model=None):
    own = trace is None
    trace = trace if trace is not None else Trace()
    if own:
        print(f"coop: trace {trace.path}")
    try:
        walk = {"long": False, "short": False, "stuck": 0, "summon_returns": 0}
        return _drive(slot, HOST_TABLE, trace, _host_check, dry_run=dry_run,
                      timeout=timeout, backend=backend, model=model, walk=walk)
    finally:
        if own:
            trace.close()


def visit(slot, relieve=None, trace=None, *, dry_run=False, timeout=600.0,
          backend=DEFAULT_BACKEND, model=None):
    if relieve == slot:
        return False, f"refusing to pause slot {slot}, the newcomer itself"
    own = trace is None
    trace = trace if trace is not None else Trace()
    if own:
        print(f"coop: trace {trace.path}")
    try:
        return _drive(slot, VISIT_TABLE, trace, _visit_check, dry_run=dry_run,
                      timeout=timeout, relief=relieve,
                      backend=backend, model=model)
    finally:
        if own:
            trace.close()


def main():
    parser = argparse.ArgumentParser(description="Decision-model co-op driver.")
    commands = parser.add_subparsers(dest="command", required=True)
    host = commands.add_parser("host", help="make the host offer a visitor slot")
    host.add_argument("slot")
    visitor = commands.add_parser("visit", help="join a host through Torii Gate")
    visitor.add_argument("slot")
    visitor.add_argument("--relieve", metavar="SLOT",
                         help="pause this other visitor after every step")
    whole = commands.add_parser("run", help="host offer, then two visitors")
    whole.add_argument("--host", required=True)
    whole.add_argument("--visitors", nargs=2, required=True, metavar=("A", "B"))
    backend = os.environ.get(BACKEND_ENV, DEFAULT_BACKEND).strip().lower()
    if backend not in BACKENDS:
        parser.error(f"{BACKEND_ENV} must be one of {', '.join(BACKENDS)}")
    for sub in (host, visitor, whole):
        sub.add_argument("--dry-run", action="store_true")
        sub.add_argument("--timeout", type=float, default=600.0)
        sub.add_argument("--backend", choices=BACKENDS, default=backend,
                         help="decision backend (default: %(default)s)")
        sub.add_argument("--model", help="override the backend model id")
    args = parser.parse_args()

    if args.command == "run":
        if len({args.host, *args.visitors}) != 3:
            parser.error("host and the two visitors must be distinct slots")
        plan = [(args.host, None), (args.visitors[0], None),
                (args.visitors[1], args.visitors[0])]
    elif args.command == "host":
        plan = [(args.slot, None)]
    else:
        plan = [(args.slot, args.relieve)]

    trace = Trace()
    print(f"coop: trace {trace.path}")
    ok, message = False, "nothing done"
    try:
        for slot, relieve in plan:
            if args.command == "host" or (args.command == "run" and slot == args.host):
                ok, message = host_offer(slot, trace, dry_run=args.dry_run,
                                         timeout=args.timeout,
                                         backend=args.backend, model=args.model)
            else:
                ok, message = visit(slot, relieve, trace, dry_run=args.dry_run,
                                    timeout=args.timeout,
                                    backend=args.backend, model=args.model)
            print(f"coop: slot {slot}: {'OK' if ok else 'FAILED'}: {message}")
            if not ok:
                break
    except RuntimeError as exc:
        ok, message = False, str(exc)
    finally:
        trace.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
