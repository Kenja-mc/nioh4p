#!/usr/bin/env python3
"""Nioh front-end for the reusable perception -> driver harness.

``perceive.py`` turns a screenshot into model answers; ``drive.py`` turns a
rule table into keys. Everything Nioh-specific is declarative in
``profiles/nioh-coop.json`` (screens, highlights, predicates, per-role rules,
safety thresholds, log oracles, relief). Behaviour that no JSON condition can
express stays here as hooks in ``GUARDS`` / ``CHECKS``, referenced by name from
the profile; a hook may raise ``drive.Stop(reason)``.

usage:
  coop.py host b                       host opens a lobby: Torii Gate, companion search
  coop.py visit a [--relieve c]        visitor joins the same way (search finds the host)
  coop.py run --host b --visitors a c  host lobby, then A and C search together
  coop.py leave b                      leave the mission (Himorogi Fragment; costs Amrita)
  coop.py decide a [--role visitor]    one frame: print perception + action
  --dry-run                            perceive once and print the action only
  --timeout SECONDS                    wall clock limit (default 600)
  --backend {mercury,luna}             default luna, or $COOP_BACKEND
  --model MODEL / --profile PATH       override model or profile

Environment: OPENROUTER_API_KEY or OPENROUTER_ENV_FILE (mercury);
EXPLABS_API_KEY or EXPLABS_KEY_FILE (luna). Keys are never printed. One JSONL
trace line per step goes to $RIG/shots/coop-<timestamp>.jsonl.
"""
import argparse
import json
import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
import perceive  # noqa: E402
from nioh_nav import RIG, SHOTS, SLOTS, resolve_slot  # noqa: E402

BACKEND_ENV = "COOP_BACKEND"
PROFILE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "profiles", "nioh-coop.json")


def load_profile(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def make_view(slot, profile, backend, model, table):
    # A second frame costs a model call and a settle. It only helps when the
    # low-confidence answer could change the action: the screen itself, or
    # the highlight on a screen whose rules test the highlight.
    keyed = {rule["screen"] for rule in table
             if isinstance(rule.get("when"), dict) and "highlight" in rule["when"]}

    def retry_if(low, answers):
        return "screen" in low or ("highlight" in low and
                                   answers["screen"].get("choice") in keyed)

    return perceive.Perceiver.from_profile(
        profile, drive.GamescopeCapture(slot), backend, model,
        prefix=f"coop-{slot}", label=f"slot {slot}", retry_if=retry_if)


def slot_log_dir(slot):
    """The directory the slot's proxy logs to: its ini out_dir, else the game dir."""
    game_dir = os.path.join(RIG, f"lan-{slot}")
    try:
        with open(os.path.join(game_dir, "nioh3p.ini"), encoding="utf-8", errors="replace") as ini:
            for line in ini:
                key, _, value = line.strip().partition("=")
                if key.strip() == "out_dir" and value.strip():
                    value = value.strip()
                    if value[:2].upper() == "Z:":   # Wine's view of the Linux root
                        value = value[2:].replace("\\", "/")
                    return value
    except OSError:
        pass
    return game_dir


def make_oracle(profile, role, slot):
    spec = profile["oracles"][role["oracle"]]
    return perceive.NeedleOracle(os.path.join(slot_log_dir(slot), spec["file"]),
                                 spec["needle"])


# ---- Nioh-specific hooks (the profile extension point) ----
def _other_limit(state, ctx):
    run = state.get("other_run", 0)
    limit = ctx["safety"]["other_limit"]
    if not state.get("join_seen") and run > limit:
        return "fail", (f"{run} consecutive unclassified 'other' frames "
                        f"(cap {limit}); no known screen is appearing")
    return "wait", None


def host_check(state, ctx):
    if state["host_offering"]:
        return "success", "host owns a companion lobby (new CreateLobby line)"
    return _other_limit(state, ctx)


def visitor_check(state, ctx):
    if state["screen"] != "in_mission":
        return _other_limit(state, ctx)
    if not state["join_seen"]:
        return "fail", ("visitor reached in_mission without a new 'Steam JoinLobby:' "
                        "line; possible solo mission, stopping (no auto recovery)")
    if state["coop_partner"] >= ctx["safety"]["predicate_min"]:
        return "success", "visitor in the host mission (JoinLobby + coop partner)"
    return "wait", None


def host_guard(state, ctx):
    """Once the lobby exists, any key could cancel the search: send none."""
    return drive.wait(3.0) if state["host_offering"] else None


def visitor_guard(state, ctx):
    """Once JoinLobby exists the join is in flight: send no keys at all."""
    return drive.wait(3.0) if state["join_seen"] else None


def leave_check(state, ctx):
    if state["screen"] in ("world_map", "map_menu", "torii_menu"):
        return "success", "out of the mission (world map or Torii Gate)"
    return _other_limit(state, ctx)


GUARDS = {"host_guard": host_guard, "visitor_guard": visitor_guard}
CHECKS = {"host_check": host_check, "visitor_check": visitor_check,
          "leave_check": leave_check}


# ---- relief: pause the other visitor(s) between steps ----
def pause_cycle(slots, seconds=5.0, pause=2.0):
    paused = []
    for slot in slots:
        marker = f"HOME={os.path.join(RIG, f'home-{slot}')}"
        drive.pause_process("nioh.exe", marker.encode(), seconds, pause,
                            lambda why: print(f"coop: relief: {why}"))
        paused.append(slot)
    return paused


# ---- roles ----
def run_role(slot, role_name, profile, trace, *, dry_run=False, timeout=None,
             backend=perceive.DEFAULT_BACKEND, model=None, relieve=None):
    relieve = list(relieve or [])
    if slot in relieve:
        return False, f"refusing to pause slot {slot}, the newcomer itself"
    if not resolve_slot(slot):
        return False, f"no live game for slot {slot}"
    role, safety = profile["roles"][role_name], profile["safety"]
    table = profile["rules"][role["rules"]]
    guard, check = role.get("guard"), role.get("check")
    for name, registry in ((guard, GUARDS), (check, CHECKS)):
        if name and name not in registry:
            raise RuntimeError(f"unknown hook {name!r} in profile")
    view = make_view(slot, profile, backend, model, table)
    oracle = make_oracle(profile, role, slot) if role.get("oracle") else None
    ctx = {"safety": safety}
    other_run = 0

    def perceive_step(step):
        nonlocal other_run
        state = view.perceive(step)
        state["slot"] = slot
        other_run = other_run + 1 if state["screen"] == "other" else 0
        state.update(other_run=other_run, host_offering=False, join_seen=False)
        if oracle:
            oracle.scan()
            state[role["flag"]] = oracle.found()
        return state

    def on_step(row):
        row["latency"] = round(row["latency"], 2)
        trace.write(**row)
        print(f"coop step {trace.n} slot={row['slot']} screen={row['screen']} "
              f"highlight={row['highlight']} action={row['action']} "
              f"coop={row['coop_partner']:.2f} "
              f"api={row['latency']:.1f}s backend={row['backend']}")

    actuator = None
    if not dry_run:
        actuator = drive.XTestActuator(SLOTS[slot]["display"])
        if relieve:
            relief = profile["relief"]
            actuator = drive.ReliefActuator(
                actuator, lambda: pause_cycle(relieve, relief["seconds"], relief["pause"]))
            print(f"coop: relief: pausing slot(s) {', '.join(relieve)} for "
                  f"{relief['seconds']:.0f} s after every step")
    driver = drive.Driver(
        table, perceive_step, actuator=actuator,
        guard=(lambda state: GUARDS[guard](state, ctx)) if guard else None,
        check=(lambda state: CHECKS[check](state, ctx)) if check else None,
        on_step=on_step, step_limit=safety["step_limit"],
        timeout=timeout if timeout is not None else safety["timeout"],
        return_un=safety["return_un"], return_settle=safety["return_settle"],
        no_progress=safety["sent_key_limit"], loading_screens=safety["loading_screens"],
        uncertain_highlight=safety.get("uncertain_highlight", "uncertain"),
        uncertain_settle=role.get("uncertain_settle"),
        uncertain_safe_keys=role.get("uncertain_safe_keys", ()), dry_run=dry_run)
    return driver.run()


def decide(slot, role_name, profile, *, backend, model):
    """Capture one frame, print perception + the planned action, send nothing."""
    if not resolve_slot(slot):
        print(f"coop: no live game for slot {slot}", file=sys.stderr)
        return 1
    role = profile["roles"][role_name]
    table = profile["rules"][role["rules"]]
    try:
        state = make_view(slot, profile, backend, model, table).perceive(0)
    except Exception as exc:
        print(f"coop: perception failed: {exc}", file=sys.stderr)
        return 1
    state.update(other_run=1 if state["screen"] == "other" else 0,
                 host_offering=False, join_seen=False)
    if role.get("flag"):
        state[role["flag"]] = False
    ctx = {"safety": profile["safety"]}
    try:
        source, rule, action = drive.plan(
            state, table,
            (lambda item: GUARDS[role["guard"]](item, ctx)) if role.get("guard") else None)
    except drive.Stop as exc:
        source, rule, action = f"stop: {exc}", None, None
    report = {"slot": slot, "role": role_name, "source": source,
              "rule": rule.name if rule else None,
              "action": action.text() if action else "none", **state}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


def main():
    parser = argparse.ArgumentParser(description="Decision-model co-op driver.")
    commands = parser.add_subparsers(dest="command", required=True)
    host = commands.add_parser("host", help="make the host offer a visitor slot")
    host.add_argument("slot")
    visitor = commands.add_parser("visit", help="join a host through Torii Gate")
    visitor.add_argument("slot")
    visitor.add_argument("--relieve", metavar="SLOT", nargs="+",
                         help="pause these other visitors after every step")
    whole = commands.add_parser("run", help="host opens a lobby, then all visitors search together")
    whole.add_argument("--host", required=True)
    whole.add_argument("--visitors", nargs="+", required=True, metavar="SLOT")
    leave = commands.add_parser("leave", help="leave the mission with the Himorogi Fragment")
    leave.add_argument("slot")
    one = commands.add_parser("decide", help="perceive one frame and print the action")
    one.add_argument("slot")
    one.add_argument("--role", choices=("host", "visitor", "leave"), default="host")
    backend = os.environ.get(BACKEND_ENV, perceive.DEFAULT_BACKEND).strip().lower()
    if backend not in perceive.BACKENDS:
        parser.error(f"{BACKEND_ENV} must be one of {', '.join(perceive.BACKENDS)}")
    for sub in (host, visitor, whole, leave, one):
        sub.add_argument("--profile", default=PROFILE_PATH)
        sub.add_argument("--backend", choices=perceive.BACKENDS, default=backend,
                         help="decision backend (default: %(default)s)")
        sub.add_argument("--model", help="override the backend model id")
    for sub in (host, visitor, whole, leave):
        sub.add_argument("--dry-run", action="store_true")
        sub.add_argument("--timeout", type=float, default=None)
    args = parser.parse_args()
    try:
        profile = load_profile(args.profile)
    except (OSError, ValueError) as exc:
        parser.error(f"cannot load profile {args.profile}: {exc}")
    if args.command == "decide":
        return decide(args.slot, args.role, profile, backend=args.backend,
                      model=args.model)
    # Waves run in order; the roles inside one wave run at the same time. The
    # companion lobby only takes the visitors that arrive before its
    # preparation countdown ends, so all visitors must search together.
    if args.command == "run":
        if len({args.host, *args.visitors}) != 1 + len(args.visitors):
            parser.error("host and visitors must be distinct slots")
        waves = [[(args.host, "host", None)],
                 [(slot, "visitor", None) for slot in args.visitors]]
    elif args.command == "host":
        waves = [[(args.slot, "host", None)]]
    elif args.command == "leave":
        waves = [[(args.slot, "leave", None)]]
    else:
        waves = [[(args.slot, "visitor", args.relieve)]]
    trace = perceive.Trace(SHOTS)
    print(f"coop: trace {trace.path}")
    results = []

    def one(slot, role_name, relieve):
        try:
            ok, message = run_role(slot, role_name, profile, trace,
                                   dry_run=args.dry_run, timeout=args.timeout,
                                   backend=args.backend, model=args.model,
                                   relieve=relieve)
        except RuntimeError as exc:
            ok, message = False, str(exc)
        print(f"coop: slot {slot}: {'OK' if ok else 'FAILED'}: {message}")
        results.append(ok)

    try:
        for wave in waves:
            threads = [threading.Thread(target=one, args=item) for item in wave]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            if not all(results):
                break
    finally:
        trace.close()
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
