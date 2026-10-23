#!/usr/bin/env python3
"""Nioh front-end for the reusable perception -> driver harness.

``perceive.py`` turns a screenshot into model answers; ``drive.py`` turns a
rule table into keys. Everything Nioh-specific is declarative in
``profiles/nioh-coop.json`` (screens, highlights, predicates, per-role rules,
safety thresholds, log oracles, relief). Behaviour that no JSON condition can
express stays here as hooks in ``GUARDS`` / ``CHECKS``, referenced by name from
the profile; a hook may raise ``drive.Stop(reason)``.

usage:
  coop.py host b                       host offers co-op at the starting shrine
  coop.py visit a [--relieve b c]      visitor joins through Torii Gate
  coop.py run --host b --visitors a c  host offer, then A, then C (A paused)
  coop.py run --host b --visitors a c d   also D (A and C paused)
  coop.py decide a [--role visitor]    one frame: print perception + action
  --dry-run                            perceive once and print the action only
  --timeout SECONDS                    wall clock limit (default 600)
  --backend {mercury,luna}             default mercury, or $COOP_BACKEND
  --model MODEL / --profile PATH       override model or profile

Environment: OPENROUTER_API_KEY or OPENROUTER_ENV_FILE (mercury);
EXPLABS_API_KEY or EXPLABS_KEY_FILE (luna). Keys are never printed. One JSONL
trace line per step goes to $RIG/shots/coop-<timestamp>.jsonl.
"""
import argparse
import json
import os
import sys

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


def make_view(slot, profile, backend, model):
    return perceive.Perceiver.from_profile(
        profile, drive.GamescopeCapture(slot), backend, model,
        prefix=f"coop-{slot}", label=f"slot {slot}")


def make_oracle(profile, role, slot):
    spec = profile["oracles"][role["oracle"]]
    path = spec["file"].format(slot=slot, rig=RIG).replace("${RIG}", RIG).replace("$RIG", RIG)
    return perceive.NeedleOracle(os.path.expanduser(os.path.expandvars(path)),
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
    if not state["host_offering"]:
        return _other_limit(state, ctx)
    if state["screen"] == "in_mission":
        return "success", ("host offering active (new CreateLobby line) and back "
                           "in the mission")
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
    """Never Return once the lobby exists; walk fallback at the shrine."""
    if state["host_offering"] and state["screen"] != "in_mission":
        return drive.tap("BackSpace", 90, 0.6)
    if (state["screen"] == "in_mission"
            and state["pray_prompt"] < ctx["safety"]["predicate_min"]):
        return _host_walk(ctx)
    return None


def visitor_guard(state, ctx):
    """Once JoinLobby exists the join is in flight: send no keys at all."""
    return drive.wait(3.0) if state["join_seen"] else None


def _host_walk(ctx):
    """At most one long walk, then one short retry, then a clean failure."""
    walk, cfg = ctx["walk"], ctx["safety"]["host_walk"]
    if not walk["long"]:
        walk["long"] = True
        return drive.tap("w", cfg["long_ms"], 1.0)
    walk["stuck"] += 1
    if walk["short"]:
        raise drive.Stop(f"host shows in_mission with no PRAY prompt after a "
                         f"{cfg['long_ms']} ms and a {cfg['short_ms']} ms walk; "
                         f"stopping instead of walking away from the shrine")
    if walk["stuck"] >= cfg["stuck"]:
        walk["short"] = True
        return drive.tap("w", cfg["short_ms"], 1.0)
    return drive.wait(1.0)


GUARDS = {"host_guard": host_guard, "visitor_guard": visitor_guard}
CHECKS = {"host_check": host_check, "visitor_check": visitor_check}


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
    table = profile["rules"][role_name]
    guard, check = role.get("guard"), role.get("check")
    for name, registry in ((guard, GUARDS), (check, CHECKS)):
        if name and name not in registry:
            raise RuntimeError(f"unknown hook {name!r} in profile")
    view = make_view(slot, profile, backend, model)
    oracle = make_oracle(profile, role, slot)
    ctx = {"safety": safety, "walk": {"long": False, "short": False, "stuck": 0}}
    other_run = 0

    def perceive_step(step):
        nonlocal other_run
        state = view.perceive(step)
        state["slot"] = slot
        oracle.scan()
        other_run = other_run + 1 if state["screen"] == "other" else 0
        state.update(other_run=other_run, host_offering=False, join_seen=False)
        state[role["flag"]] = oracle.found()
        return state

    def on_step(row):
        row["summon_returns"] = row["counts"].get("summon", 0)
        row["latency"] = round(row["latency"], 2)
        trace.write(**row)
        print(f"coop step {trace.n} slot={row['slot']} screen={row['screen']} "
              f"highlight={row['highlight']} action={row['action']} "
              f"pray={row['pray_prompt']:.2f} coop={row['coop_partner']:.2f} "
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
    role, table = profile["roles"][role_name], profile["rules"][role_name]
    try:
        state = make_view(slot, profile, backend, model).perceive(0)
    except Exception as exc:
        print(f"coop: perception failed: {exc}", file=sys.stderr)
        return 1
    state.update(other_run=1 if state["screen"] == "other" else 0,
                 host_offering=False, join_seen=False)
    state[role["flag"]] = False
    ctx = {"safety": profile["safety"], "walk": {"long": False, "short": False,
                                                 "stuck": 0}}
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
    whole = commands.add_parser("run", help="host offer, then two or three visitors")
    whole.add_argument("--host", required=True)
    whole.add_argument("--visitors", nargs="+", required=True, metavar=("A", "B"))
    one = commands.add_parser("decide", help="perceive one frame and print the action")
    one.add_argument("slot")
    one.add_argument("--role", choices=("host", "visitor"), default="host")
    backend = os.environ.get(BACKEND_ENV, perceive.DEFAULT_BACKEND).strip().lower()
    if backend not in perceive.BACKENDS:
        parser.error(f"{BACKEND_ENV} must be one of {', '.join(perceive.BACKENDS)}")
    for sub in (host, visitor, whole, one):
        sub.add_argument("--profile", default=PROFILE_PATH)
        sub.add_argument("--backend", choices=perceive.BACKENDS, default=backend,
                         help="decision backend (default: %(default)s)")
        sub.add_argument("--model", help="override the backend model id")
    for sub in (host, visitor, whole):
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
    if args.command == "run":
        if len(args.visitors) not in (2, 3):
            parser.error("run needs exactly two or three visitors")
        if len({args.host, *args.visitors}) != len(args.visitors) + 1:
            parser.error("the host and the visitors must be distinct slots")
        steps = [(args.host, "host", None), (args.visitors[0], "visitor", None),
                 (args.visitors[1], "visitor", [args.visitors[0]])]
        if len(args.visitors) == 3:
            steps.append((args.visitors[2], "visitor", args.visitors[:2]))
    elif args.command == "host":
        steps = [(args.slot, "host", None)]
    else:
        steps = [(args.slot, "visitor", args.relieve)]
    trace = perceive.Trace(SHOTS)
    print(f"coop: trace {trace.path}")
    ok, message = False, "nothing done"
    try:
        for slot, role_name, relieve in steps:
            ok, message = run_role(slot, role_name, profile, trace,
                                   dry_run=args.dry_run, timeout=args.timeout,
                                   backend=args.backend, model=args.model,
                                   relieve=relieve)
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
