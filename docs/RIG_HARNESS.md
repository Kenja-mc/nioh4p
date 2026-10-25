# Rig harness: perceive -> drive -> profile

The co-op driver is split into two reusable modules that do not depend on the UI, plus a profile that holds everything game-specific. To add another game or window manager, write a profile (or a ~30-line script) instead of editing the loop. The table below lists the four files and their roles.

| file | role |
|---|---|
| `tools/rig/perceive.py` | screenshot -> decision-model answers: `ask_decisions`, `Perceiver`, `Trace`, `TailOracle`, confidence helpers |
| `tools/rig/drive.py` | answers -> keys: `Action`, `Rule`, `Driver`, actuators, capturers |
| `tools/rig/profiles/nioh-coop.json` | Nioh screens, highlights, predicates, rules, thresholds, oracles, relief |
| `tools/rig/coop.py` | Nioh CLI and the few named Python hooks the profile references |

The loop works in this order. Capture a frame, then ask the model about it. Degrade uncertain answers to safe defaults, then pick the first matching rule. Send the keys of that rule or wait. The model only classifies, and it never chooses a keypress.

## Adding a game

Use a command capturer and a command actuator. No Python abstraction is needed. The script below drives a window with ImageMagick and xdotool.

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
perceiver = perceive.Perceiver(capture, ask, "luna", confidence_min=0.6,
                               fallbacks={"screen": "other", "highlight": "none"},
                               prefix="game", label="game window")
check = lambda state: (("success", "in game") if state["screen"] == "play"
                       else ("wait", None))
driver = drive.Driver(rules, perceiver.perceive, actuator=actuator, check=check,
                      on_step=lambda row: print(row["step"], row["action"]),
                      loading_screens=["other"])
print(driver.run())
```

`CommandCapture` substitutes `{name}` and `{path}`. The actuator substitutes `{key}`, `{ms}` and `{seconds}`. For a hold, use a separate template, for example `CommandActuator("xdotool key --delay {ms} {key}", "xdotool keydown {key} && sleep {seconds} && xdotool keyup {key}")`.

The profile route is the same, with data in JSON. The members are `screens` and `highlights` (choice questions), `predicates`, `fallbacks`, `safety`, `oracles`, top-level `rules.host` and `rules.visitor`, and `roles`. Each role has an oracle name, the state flag that it sets, and `guard` and `check` hook names. Then call `perceive.Perceiver.from_profile(profile, capture, ...)` and `drive.Driver(profile["rules"]["host"], ...)`.

## Question specs

Pass a mapping of question name to spec. The example below gives three questions. Each name maps to one question object.

```json
{"screen":  {"type": "choice", "instructions": "One sentence.",
             "choices": [{"value": "title", "description": "Title menu"}]},
 "highlight": {"type": "choice", "instructions": "...",
               "criteria": {"continue": "CONTINUE is highlighted"}},
 "without_conditions": {"type": "predicate", "instructions": "Yes/no question"}}
```

`mercury` sends choices as the `criteria` map and predicates as `noul`. `luna` sends `choices` and `predicate`. Give either form or both, and the missing form is derived. Each backend gets one image and the profile prompt. A `.jpg` path is sent as `image/jpeg` and anything else as `image/png`. `GamescopeCapture` converts each frame to a quality-85 JPEG, which halves the latency at the same answers. `ask_decisions` returns `{"answers", "raw", "screenshot", "latency", "backend", "model", "usage"}`. `raw` keeps the answer objects of the API. Unknown choices raise an error instead of being executed.

## Confidence and safety

- A choice below `confidence_min` (0.6) is re-shot once after 1 s when `Perceiver(retry_if=...)` says a second frame can change the action. The default retries any low choice. `coop.py` retries a low `screen`, and a low `highlight` only on screens whose rules test the highlight. If the answer is still low, the driver substitutes the configured fallback: `screen -> other` (wait) and `highlight -> uncertain`. No `when` that names a real highlight matches then.
- Rules that send a dangerous key must also require `{"highlight": ["..."], "confidence_min": 0.9}`. `confidence_min` compares the highlight confidence. `{"without_conditions": {"max": 0.5}}` compares a predicate.
- `Driver(uncertain_settle=1.5, uncertain_safe_keys=["BackSpace", "Up"])` replaces uncertain key sends with a wait, except for the safe navigation keys. Enable it for each profile role. The Nioh visitor uses it.
- `Driver` also enforces the Return debounce (3 unchanged frames), a no-progress cap on sent keys, the step limit and the wall timeout. Hooks can raise `drive.Stop(reason)` to end the run cleanly.

## Trace

The trace has one JSON line per step, in `$RIG/shots/coop-<timestamp>.jsonl`. The fields are `step`, `slot`, `screenshot`, `screen`, `highlight`, `answers`, `raw`, `screen_confidence`, `highlight_confidence`, one field per profile predicate (for Nioh `without_conditions` and `coop_partner`), `action`, `latency`, `backend`, `model`, `usage`, `dry_run`, `rule`, `counts`, `host_offering`, `join_seen` and `other_run`. All visitors of one `run` write to the same trace file.

## Smoke check

Run the two commands below to make sure that a profile is correct before the loop sends keys. Both commands send no keys. The output shows what the model reported and what the driver selected.

```sh
python3 tools/rig/coop.py decide a --role visitor   # prints perception + action, sends nothing
python3 tools/rig/coop.py host b --dry-run          # one full driven step, no keys
```

`decide` exits 0 after it prints JSON: the screen and highlight of the model with confidences, the predicates, the raw answers, and the action that the guard or table selects. Use it to make sure that a new profile is correct before you let the loop send keys.
