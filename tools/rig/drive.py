#!/usr/bin/env python3
"""Execution half of the rig harness: actions, rules, and the drive loop.

UI-agnostic. A profile supplies rules whose ``when`` is JSON; callers plug in
capture and input through the actuator/capturer classes or their own callables.

Public pieces:
  Action / tap() / hold() / wait()   one tap, one hold, one sleep
  Rule(screen, action, when=None)   one table row; ``limit`` caps sends per run
  first_match(table, state, counts) the first matching rule (or None)
  condition_met(condition, state)   evaluates declarative conditions
  Driver(...).run() -> (ok, reason)
  BaseActuator, XTestActuator, CommandActuator, GamescopeCapture, CommandCapture
  Stop                              raise it in a hook to end the run cleanly

Conditions are JSON: a value is an equality test, a list is membership, a
``{"min"/"max"/"in"/"not_in"/"equals"}`` object compares, ``confidence_min``
compares the state's ``highlight_confidence``, and ``all`` / ``any`` / ``not``
compose. A callable is also accepted.

Driver hooks:
  check(state) -> ("success" | "fail" | "wait", reason)
  guard(state) -> Action | None      runs before the table
  on_step(row)                       one mutable trace row per step
"""
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

RETURN_UNCHANGED = 3
RETURN_SETTLE = 1.5


class Stop(Exception):
    """A hook's clean way to end the run: Driver returns (False, str(exc))."""


# ---- actions ----
@dataclass(frozen=True)
class Action:
    kind: str
    key: str = None
    ms: int = 90
    seconds: float = 0.0
    settle: float = 0.0

    @property
    def sends_key(self):
        return self.kind in ("tap", "hold")

    def text(self):
        if self.kind == "wait":
            return f"wait {self.seconds:.1f}s"
        if self.kind == "hold":
            return f"hold {self.key} {self.seconds:.1f}s"
        return f"tap {self.key} {self.ms}ms"


def tap(key, ms=90, settle=0.8):
    return Action("tap", key, ms, 0.0, settle)


def hold(key, seconds=1.7, settle=1.5):
    return Action("hold", key, 90, seconds, settle)


def wait(seconds=4.0):
    return Action("wait", None, 90, seconds, seconds)


def as_action(value):
    if isinstance(value, Action):
        return value
    if isinstance(value, str):
        return tap(value)
    if not isinstance(value, dict):
        raise TypeError(f"action must be an Action, str or dict, not {type(value).__name__}")
    kind = value.get("kind")
    if kind == "wait":
        seconds = float(value.get("seconds", 4.0))
        return wait(seconds) if "settle" not in value else Action(
            "wait", None, 90, seconds, float(value["settle"]))
    if kind == "hold":
        return hold(value["key"], float(value.get("seconds", 1.7)),
                    float(value.get("settle", 1.5)))
    if kind == "tap":
        return tap(value["key"], int(value.get("ms", 90)),
                   float(value.get("settle", 0.8)))
    raise ValueError(f"unknown action kind {kind!r}")


# ---- rules ----
@dataclass(frozen=True)
class Rule:
    screen: str
    action: Action
    when: object = None
    name: str = None
    limit: int = None

    @classmethod
    def from_dict(cls, data):
        if isinstance(data, cls):
            return data
        name = data.get("name")
        limit = data.get("limit")
        if limit is not None and not name:
            raise ValueError("a limited rule needs a name")
        return cls(screen=data.get("screen"), action=as_action(data["action"]),
                   when=data.get("when"), name=name, limit=limit)


def as_rule(value):
    return value if isinstance(value, Rule) else Rule.from_dict(value)


def condition_met(condition, state):
    if condition is None or condition is True:
        return True
    if callable(condition):
        return bool(condition(state))
    if isinstance(condition, str):
        return bool(state.get(condition))
    if not isinstance(condition, dict):
        raise TypeError(f"condition must be a dict or callable, not {type(condition).__name__}")
    if "all" in condition:
        return all(condition_met(item, state) for item in condition["all"])
    if "any" in condition:
        return any(condition_met(item, state) for item in condition["any"])
    if "not" in condition:
        return not condition_met(condition["not"], state)
    for field, expected in condition.items():
        if field == "confidence_min":
            if float(state.get("highlight_confidence") or 0.0) < expected:
                return False
            continue
        actual = state.get(field)
        if isinstance(expected, dict):
            if "min" in expected and not (actual is not None and actual >= expected["min"]):
                return False
            if "max" in expected and not (actual is not None and actual <= expected["max"]):
                return False
            if "in" in expected and actual not in expected["in"]:
                return False
            if "not_in" in expected and actual in expected["not_in"]:
                return False
            if "equals" in expected and actual != expected["equals"]:
                return False
        elif isinstance(expected, (list, tuple, set)):
            if actual not in expected:
                return False
        elif actual != expected:
            return False
    return True


def first_match(table, state, counts=None):
    counts = counts or {}
    for value in table:
        rule = as_rule(value)
        if rule.screen is not None and rule.screen != state.get("screen"):
            continue
        if rule.limit is not None and counts.get(rule.name, 0) >= rule.limit:
            continue
        if not condition_met(rule.when, state):
            continue
        return rule
    return None


def plan(state, table, guard=None, counts=None):
    """Guard first, then the table: (source, rule, action)."""
    if guard is not None:
        action = guard(state)
        if action is not None:
            return "guard", None, action
    rule = first_match(table, state, counts)
    return ("table", rule, rule.action) if rule is not None else (None, None, None)


# ---- actuators ----
class BaseActuator:
    def tap(self, key, ms=90):
        raise NotImplementedError

    def hold(self, key, seconds):
        raise NotImplementedError

    def perform(self, action):
        if action.kind == "tap":
            self.tap(action.key, action.ms)
        elif action.kind == "hold":
            self.hold(action.key, action.seconds)
        elif action.kind != "wait":
            raise ValueError(f"unknown action kind {action.kind!r}")
        time.sleep(action.settle)


class XTestActuator(BaseActuator):
    """XTest key injection, using the rig's nioh_nav.Input."""

    def __init__(self, display):
        from nioh_nav import Input
        self.input = Input(display)

    def tap(self, key, ms=90):
        self.input.tap(key, ms)

    def hold(self, key, seconds):
        self.input.hold(key, seconds)


class CommandActuator(BaseActuator):
    """Shell command template with {key}, {ms}, {seconds} substituted."""

    def __init__(self, template, hold_template=None, timeout=15.0):
        self.template = template
        self.hold_template = hold_template or template
        self.timeout = timeout

    def _run(self, command):
        result = subprocess.run(command, shell=True, capture_output=True,
                                text=True, timeout=self.timeout)
        if result.returncode:
            detail = (result.stderr or result.stdout).strip()[:200]
            raise RuntimeError(f"actuator command failed ({result.returncode}): {detail}")

    def tap(self, key, ms=90):
        self._run(self.template.format(key=key, ms=ms, seconds=ms / 1000.0))

    def hold(self, key, seconds):
        self._run(self.hold_template.format(key=key, ms=int(seconds * 1000),
                                           seconds=seconds))


def process_pid(comm, environ_entry):
    """PID of the process named ``comm`` whose environment contains a marker."""
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            with open(f"{entry.path}/comm") as handle:
                if handle.read().strip() != comm:
                    continue
            with open(f"{entry.path}/environ", "rb") as handle:
                if environ_entry in handle.read().split(b"\0"):
                    return int(entry.name)
        except OSError:
            continue
    return None


def pause_process(comm, environ_entry, seconds=5.0, pause=2.0, on_error=None):
    """SIGSTOP the matching process, wait, SIGCONT, then wait ``pause`` again."""
    pid = process_pid(comm, environ_entry)
    if pid is None:
        if on_error:
            on_error(f"no live {comm} for that slot")
        return False
    try:
        os.kill(pid, signal.SIGSTOP)
    except OSError as exc:
        if on_error:
            on_error(f"SIGSTOP ({pid}) failed: {exc}")
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


class ReliefActuator(BaseActuator):
    """Run ``pause()`` after every action (e.g. SIGSTOP another game)."""

    def __init__(self, inner, pause):
        self.inner = inner
        self.pause = pause

    def tap(self, key, ms=90):
        self.inner.tap(key, ms)

    def hold(self, key, seconds):
        self.inner.hold(key, seconds)

    def perform(self, action):
        self.inner.perform(action)
        self.pause()


# ---- capturers ----
class GamescopeCapture:
    """Screenshot the rig slot's gamescope compositor."""

    def __init__(self, slot):
        self.slot = slot

    def __call__(self, name, settle=0.5):
        from nioh_nav import shot
        return shot(self.slot, name, settle)


class CommandCapture:
    """Screenshot anything else; command gets {path} and {name} substituted."""

    def __init__(self, command, path, settle=0.5, timeout=20.0):
        self.command = command
        self.path = path
        self.settle = settle
        self.timeout = timeout

    def __call__(self, name, settle=None):
        path = self.path.format(name=name)
        time.sleep(self.settle if settle is None else settle)
        if os.path.exists(path):
            os.unlink(path)
        result = subprocess.run(self.command.format(path=path, name=name),
                                shell=True, capture_output=True, text=True,
                                timeout=self.timeout)
        if result.returncode:
            raise RuntimeError(f"capture command failed ({result.returncode})")
        if not os.path.exists(path) or os.path.getsize(path) == 0:
            return None
        return path


# ---- driver ----
class Driver:
    def __init__(self, table, perceive, *, actuator=None, guard=None, check=None,
                 on_step=None, step_limit=60, timeout=600.0,
                 return_key="Return", return_un=RETURN_UNCHANGED,
                 return_settle=RETURN_SETTLE, no_progress=6,
                 loading_screens=(), uncertain_highlight="uncertain",
                 uncertain_settle=None, uncertain_safe_keys=(), dry_run=False):
        self.table = [as_rule(value) for value in table]
        self.perceive = perceive
        self.actuator = actuator
        self.guard = guard
        self.check = check
        self.on_step = on_step
        self.step_limit = step_limit
        self.timeout = timeout
        self.return_key = return_key
        self.return_un = return_un
        self.return_settle = return_settle
        self.no_progress = no_progress
        self.loading_screens = set(loading_screens)
        self.uncertain_highlight = uncertain_highlight
        self.uncertain_settle = uncertain_settle
        self.uncertain_safe_keys = set(uncertain_safe_keys)
        self.dry_run = dry_run
        self.counts = {}

    def run(self):
        deadline = time.monotonic() + self.timeout
        last_key = None
        sent_same = 0
        retn_key = None
        retn_count = 0
        for step in range(self.step_limit):
            if time.monotonic() > deadline:
                return False, f"wall timeout after {self.timeout:.0f}s"
            try:
                state = self.perceive(step)
            except Exception as exc:
                return False, f"perception failed: {exc}"
            stop = None
            try:
                verdict, why = self.check(state) if self.check else ("wait", None)
                _, rule, action = plan(state, self.table, self.guard, self.counts)
            except Stop as exc:
                verdict, why, stop, rule, action = "wait", None, str(exc), None, None

            # Return debounce: an unchanged frame after Return gets a wait, not
            # another Return; resend only after RETURN_UNCHANGED such frames.
            key = (state.get("screen"), state.get("highlight"))
            sends_return = (action is not None and action.kind == "tap"
                            and action.key == self.return_key)
            if retn_key == key:
                if sends_return:
                    retn_count += 1
                    if retn_count < self.return_un:
                        action = wait(self.return_settle)
                        sends_return = False
            elif retn_key is not None:
                retn_key = None
                retn_count = 0

            # An uncertain highlight never sends a key except safe moves.
            if (self.uncertain_settle is not None
                    and state.get("highlight") == self.uncertain_highlight
                    and action is not None and action.sends_key
                    and action.key not in self.uncertain_safe_keys):
                action = wait(self.uncertain_settle)

            if key != last_key:
                sent_same = 0
            sends_key = (action is not None and action.sends_key
                         and state.get("screen") not in self.loading_screens)
            text = action.text() if action is not None else "none"
            row = dict(state)
            row.update(step=step + 1, action=text,
                       rule=rule.name if rule is not None else None,
                       counts=dict(self.counts), dry_run=self.dry_run)
            if self.on_step:
                self.on_step(row)
            if verdict == "success":
                return True, why
            if verdict == "fail":
                return False, why
            if stop:
                return False, stop
            if self.dry_run:
                return True, f"dry-run: {text} in {state.get('screen')}"
            if action is None:
                return False, (f"unexpected state {state.get('screen')}/"
                               f"{state.get('highlight')} has no rule")
            if self.actuator is None:
                raise RuntimeError("driver has no actuator")
            self.actuator.perform(action)
            if action.sends_key:
                if rule is not None and rule.name:
                    self.counts[rule.name] = self.counts.get(rule.name, 0) + 1
                if sends_return:
                    retn_key = key
                    retn_count = 0
                if sends_key:
                    sent_same += 1
                    if sent_same >= self.no_progress:
                        return False, (f"no progress: {state.get('screen')}/"
                                       f"{state.get('highlight')} after {sent_same} "
                                       f"keys with no screen change (last action "
                                       f"{text!r})")
            last_key = key
        return False, f"step limit {self.step_limit} reached"
