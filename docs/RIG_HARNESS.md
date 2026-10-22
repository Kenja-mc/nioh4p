# Rig harness: perceive -> drive -> profile

The co-op driver is split into two reusable, UI-agnostic modules plus a profile
that holds everything game-specific. Adding another game or window manager
means writing a profile (or a ~30-line script), not editing the loop.

| file | role |
|---|---|
| `tools/rig/perceive.py` | screenshot -> decision-model answers: `ask_decisions`, `Perceiver`, `Trace`, `TailOracle`, confidence helpers |
| `tools/rig/drive.py` | answers -> keys: `Action`, `Rule`, `Driver`, actuators, capturers |
| `tools/rig/profiles/nioh-coop.json` | Nioh screens, highlights, predicates, rules, thresholds, oracles, relief |
| `tools/rig/coop.py` | Nioh CLI and the few named Python hooks the profile references |

The loop is: capture a frame, ask the model about it, degrade uncertain
answers to safe defaults, pick the first matching rule, then send that rule's
keys or wait. The model only classifies; it never chooses a keypress.

## Adding a game

Use a command capturer and a command actuator; no Python abstraction needed:

```python
import drive, perceive

capture = drive.CommandCapture("magick import -window root -display :1 {path}",
                               "/tmp/game-{name}.png", settle=0.6)
actuator = drive.CommandActuator("xdotool key --delay {ms} {key}")
ask = {
    "screen": {"type": "choice", "instructions": "Which screen is this?",
               "choices": [{"value": "title", "description": "Title menu"},
                           {"value": "play", "description": "In game"},
                           {"value": "other", "description": "Anything else"}]},
    "highlight": {"type": "choice", "instructions": "What is highlighted?",
                  "choices": [{"value": "continue", "description": "CONTINUE"},
                              {"value": "none", "description": "Nothing"}]},
    "prompt_visible": {"type": "predicate", "instructions": "Is a prompt visible?"},
}
rules = [
    drive.Rule("title", drive.tap("Return", settle=1.0),
               {"highlight": ["continue"], "confidence_min": 0.9}),
    drive.Rule("title", drive.tap("Down", settle=0.5)),
    drive.Rule("play", drive.wait(2.0)),
]
perceiver = perceive.Perceiver(capture, ask, "mercury", confidence_min=0.6,
                               fallbacks={"screen": "other", "highlight": "none"},
                               prefix="game", label="game window")
check = lambda state: (("success", "in game") if state["screen"] == "play"
                       else ("wait", None))
driver = drive.Driver(rules, perceiver.perceive, actuator=actuator, check=check,
                      on_step=lambda row: print(row["step"], row["action"]),
                      loading_screens=["other"])
print(driver.run())
```

`CommandCapture` substitutes `{name}` and `{path}`; the actuator substitutes
`{key}`, `{ms}` and `{seconds}`. For a hold use a separate template, e.g.
`CommandActuator("xdotool key --delay {ms} {key}",
                 "xdotool keydown {key} && sleep {seconds} && xdotool keyup {key}")`.

The profile route is the same, with data in JSON: `screens` and `highlights`
(choice questions), `predicates`, `fallbacks`, `safety`, `oracles`, top-level
`rules.host` / `rules.visitor`, and `roles` (each with an oracle name, the state
flag it sets, and `guard`/`check` hook names). Then
`perceive.Perceiver.from_profile(profile, capture, ...)` and
`drive.Driver(profile["rules"]["host"], ...)`.

## Question specs

Pass a mapping of question name to spec:

```json
{"screen":  {"type": "choice", "instructions": "One sentence.",
             "choices": [{"value": "title", "description": "Title menu"}]},
 "highlight": {"type": "choice", "instructions": "...",
               "criteria": {"continue": "CONTINUE is highlighted"}},
 "pray_prompt": {"type": "predicate", "instructions": "Yes/no question"}}
```

`mercury` sends choices as the `criteria` map and predicates as `noul`;
`luna` sends `choices` and `predicate`. Give either form (or both) and the
other is derived. Each backend gets one image and the profile prompt;
`ask_decisions` returns `{"answers", "raw", "screenshot", "latency",
"backend", "model", "usage"}`. `raw` keeps the API's own answer objects, and
unknown choices raise rather than being executed.

## Confidence and safety

- A choice below `confidence_min` (0.6) is re-shot once after 1 s. If it is
  still low, the configured fallback is substituted: `screen -> other` (wait)
  and `highlight -> uncertain` (no `when` naming a real highlight matches).
- Rules that send a dangerous key should also require
  `{"highlight": ["..."], "confidence_min": 0.9}`. `confidence_min` compares
  the highlight confidence; `{"pray_prompt": {"min": 0.5}}` compares a
  predicate.
- `Driver(uncertain_settle=1.5, uncertain_safe_keys=["BackSpace", "Up"])`
  replaces uncertain key sends with a wait except for the safe navigation
  keys. Enable it per profile role; the Nioh visitor uses it.
- `Driver` also enforces the Return debounce (3 unchanged frames), a
  no-progress cap on sent keys, the step limit, and the wall timeout. Hooks
  may raise `drive.Stop(reason)` to end cleanly.

## Trace

One JSON line per step, in `$RIG/shots/coop-<timestamp>.jsonl`. Existing field
names are unchanged: `step`, `slot`, `screenshot`, `screen`, `highlight`,
`answers`, `raw`, `screen_confidence`, `highlight_confidence`, `pray_prompt`,
`coop_partner`, `action`, `latency`, `backend`, `model`, `usage`, `dry_run`,
`host_offering`, `summon_returns`. Extra fields (`rule`, `counts`, `join_seen`,
`other_run`) can be ignored by older readers.

## Smoke check

```sh
python3 tools/rig/coop.py decide a --role visitor   # prints perception + action, sends nothing
python3 tools/rig/coop.py host b --dry-run          # one full driven step, no keys
```

`decide` exits 0 after printing JSON: the model's screen/highlight with
confidences, predicates, raw answers, and the action the guard/table would
take. Use it to verify a new profile before letting the loop send keys.
