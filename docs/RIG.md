# The headless test rig (`tools/rig/`)

Two extra Nioh instances that run with **no window on the desktop, no use of the
dGPU, and no writes anywhere in the user's Steam root, library or Proton
prefix**.  That is what the three-player tests need: a host and a second player
that can be brought up on demand, while the machine stays usable.

```
$RIG = ~/nioh3p-test
├── home-a/.local/share/Steam/     slot a: own Steam root (COW clone of the client)
│     ├── steamapps/common/Nioh/   the game, with dinput8.dll + nioh3p.ini (mod)
│     ├── steamapps/common/Proton - Experimental
│     └── steamapps/compatdata/485510/pfx   own Wine prefix, own saves, own config
├── home-b/…                       slot b: same, stock game (no mod DLL)
├── lan-a/, lan-b/, lan-c/         separate LAN game copies, all with the proxy
├── compat-c/pfx                  third LAN prefix (not inside a Steam root)
├── home-c/                       third instance's private HOME
├── logs-a/, logs-b/, logs-c/      research-era logs (out_dir); player ini logs to lan-*/
├── run-a/, run-b/, run-c/         per-slot XDG_RUNTIME_DIR
└── shots/                         screenshots taken from the rig display
```

Steam's UI uses `Xvfb :9` for slot a and `:10` for slot b. The game must instead
run inside headless gamescope: Nioh's Vulkan presentation failed on Xvfb.
DXVK's filter selects the iGPU for the game, and `--prefer-vk-device` selects it
for the compositor. Each slot has a separate Steam root, game clone and prefix;
btrfs reflinks keep rig writes separate from the original installation.

## Running it

```sh
tools/rig/setup.sh            # build/refresh the clones (idempotent, fast)
tools/rig/login.sh a          # start slot a's client and pick its account tile
tools/rig/run.sh a client     # the same client start, without the tile click
tools/rig/run.sh a game       # request launch through the running Steam client
tools/rig/run.sh a lan        # prepared LAN game; cloned native client must be running
tools/rig/run.sh c lan        # compat-c + lan-c, reusing slot b's cloned Proton/client files
tools/rig/shot.sh a name      # screenshot slot a -> $RIG/shots/name.png
DISPLAY=:9 python3 tools/rig/xkey.py key Return Down space
DISPLAY=:9 python3 tools/rig/xkey.py move 535 384 && … click 1
```

Start the client before `run.sh … game`: the raw Steam binary forwards a request
to an existing client but cannot bootstrap its runtime libraries alone. A cold
raw-binary launch failed here with missing `libXtst.so.6`; `… client` uses
`steam.sh` to establish that environment.

In each rig client's Nioh Properties, select **Proton Experimental** and set
Launch Options to the following (adjust the repository path if necessary):

```sh
NIOH3P_HEADLESS=1 NIOH3P_SCOPE=0 NIOH3P_GPU=RAPHAEL_MENDOCINO NIOH3P_VK_DEVICE=1002:13c0 /home/<user>/Projects/nioh3p/tools/quiet-launch.sh %command%
```

The wrapper clears inherited display/WSI overrides before starting gamescope.
The isolated runtime has no systemd user bus, hence `NIOH3P_SCOPE=0`; this disables
the wrapper's CPU/RAM scope limits. Keep Steam Cloud disabled in the test roots.
For a diagnostic run, prepend `PROTON_LOG=1`; its log is in that slot's HOME.

Gamescope chooses its own Xwayland display: read `Starting Xwayland on :N` in
its output, then use `DISPLAY=:N` for game input. Xvfb screenshots capture Steam,
not the game. Capture the compositor's actual frame instead:

```sh
XDG_RUNTIME_DIR="$HOME/nioh3p-test/run-a" GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
  gamescopectl screenshot "$HOME/nioh3p-test/shots/game-a.png"
```

Use the Wayland display printed by gamescope if it is not `gamescope-0`.
Xwayland's XTest pointer input here uses client-window coordinates, not the
window's offset on the root. The input helper holds keys across game frames;
an instantaneous down/up was missed by Nioh's polled input.

References: [gamescope's Steam launch examples](https://github.com/ValveSoftware/gamescope#examples)
and [Proton runtime options](https://github.com/ValveSoftware/Proton#runtime-config-options).

The Steam controller notice and Nioh launcher can both require UI confirmation
before the game starts. A forwarded request, process count or nonblack Steam
screenshot is not proof of a running game.

Stored authentication may or may not survive cloning/restarting. Both accounts
required QR authentication in the first run. A QR authorizes the account selected
in the mobile app; a pre-filled desktop username does not bind the QR to it.

## Decision-model driver

`tools/rig/coop.py` replaces OCR menu matching with a screenshot → decision
model → deterministic key table loop. The model only reports what it sees
(`screen`, `highlight`, and the predicates `pray_prompt` and `coop_partner`); a
per-state table sends keys only from the verified recipes, so an unclear frame
cannot turn into an improvised keypress.

The engine is now a reusable harness: perception lives in
`tools/rig/perceive.py`, execution in `tools/rig/drive.py`, and all Nioh
choices/rules in `tools/rig/profiles/nioh-coop.json`. `coop.py` is the thin
Nioh front-end (CLI plus a few named hooks). To drive another game or window
manager, add a profile instead of editing the loop; see
[`docs/RIG_HARNESS.md`](RIG_HARNESS.md) for the question-spec format, the
`CommandCapture`/`CommandActuator` route, and confidence guidance.

Two backends, chosen with `--backend {mercury,luna}` or `COOP_BACKEND`
(default `mercury`):

- `mercury` — OpenRouter Mercury Decide (`inception/mercury-decide:free`, free
  tier, rate limited) at `https://openrouter.ai/api/alpha/decisions`. Choices
  are sent as `criteria` maps, predicates as `noul` questions.
- `luna` — Experiential Labs Decisions (`gpt-6-luna-decisions`) with named
  questions/choices and `probability` answers.

`--model` overrides the backend's model id. Both responses are normalized to
the same decision dict; the JSONL trace records `backend`, `model` and the
per-request `usage` (`input_tokens`, `output_tokens`, `cost`).

Environment variables:

- `OPENROUTER_API_KEY`, or `OPENROUTER_ENV_FILE` naming a file that holds an
  `OPENROUTER_API_KEY=` line (backend `mercury`).
- `EXPLABS_API_KEY`, or `EXPLABS_KEY_FILE` naming a file that holds it
  (backend `luna`).

Keys are never logged or printed. One request per step, re-shot once if a
choice falls below 0.6 confidence; if it is still below 0.6, `screen` degrades
to `other` (wait) and `highlight` to `uncertain` (no Return rule matches, so
only safe moves and waits fire), with the raw choice and confidence kept in the
trace. Every step appends one JSON line to
`$RIG/shots/coop-<timestamp>.jsonl` (screenshot, answers, action, latency) and
prints a one-line summary.

Accuracy on rig screenshots: Mercury Decide classified 7/7 screens and 6/6
highlighted items, and rejected 3/3 mission-node negatives (a mission node was
never mistaken for STARTING POINT), at roughly 1.2 s per request on the free
tier.

```sh
python3 tools/rig/coop.py host b
python3 tools/rig/coop.py visit a
python3 tools/rig/coop.py visit c --relieve a
python3 tools/rig/coop.py run --host b --visitors a c   # offer, A, then C (A paused)
python3 tools/rig/coop.py host b --dry-run              # perceive + print action only
python3 tools/rig/coop.py decide a --role visitor       # one frame, no keys sent
python3 tools/rig/coop.py visit a --backend luna        # use Experiential Labs
python3 tools/rig/coop.py visit a --profile my-game.json
```

`decide` captures one frame, prints the perception and the action the
guard/table would take as JSON, and exits 0 without sending anything.

Safety rules:

- On the title menu, `Return` is sent only when `highlight` is `continue` with
  confidence ≥ 0.9; `new_game` gets `Down`, `load_or_system` gets `Up`, and
  anything else (including `uncertain`) waits 1.5 s. This keeps a run from
  starting a new game.
- On `world_map`, `Return` is sent only when `highlight` is
  `starting_point_node` with confidence ≥ 0.9. Every other world-map frame gets
  `BackSpace`: the footer's `B: STARTING POINT` binding jumps the cursor onto
  the starting point instead of cycling node by node. This prevents a visitor
  from starting a solo mission.
- A visitor confirms a `dialog` only when `highlight` is `confirm_button`
  (the boot autosave notice); every other dialog frame is cancelled with
  `BackSpace`, because the recipe never needs YES and a stray one could start a
  solo mission. The host keeps confirming dialogs with `Return`.
- The host taps `w` at most once per run (300 ms) when `in_mission` shows no
  PRAY prompt, then waits 1 s and re-perceives; after 4 such perceptions it
  taps `w` for 150 ms once more, then fails with a clear message rather than
  walking away from the shrine.
- A visitor that shows `in_mission` without a new `Steam JoinLobby:` log line is
  reported and the run stops; there is no automatic recovery from a solo
  mission.
- Once a visitor's `Steam JoinLobby:` line exists, the driver sends no keys at
  all: every screen waits 3 s until success or timeout. The world map can
  reappear during the join load, where `Return` would be unsafe.
- After any `Return`, an unchanged `(screen, highlight)` gets a 1.5 s wait
  instead of another `Return`; the key is resent only after 3 unchanged
  perceptions (the game may have dropped it). This applies to both tables.
- The host offering is confirmed by a new
  `Steam_ISteamMatchmaking::CreateLobby` line in `$RIG/lan-b/nioh3p.log` **and**
  a later `in_mission` frame, not by the menu closing. Once that line exists
  the host never sends `Return` again: every non-gameplay frame gets
  `BackSpace` until the mission is visible.
- On the Summon Visitor plaque the host sends `Return` at most twice per run
  (offer + confirm); after that the state gets `BackSpace` or a wait. The
  `summon_status` panel (`Invoke Visitor … A: STOP OFFERING, B: BACK`) is
  always exited with `BackSpace`, never `Return`, so the offer is not stopped.
- `--relieve` pauses the *other* visitor's `nioh.exe` (matched by rig HOME) with
  `SIGSTOP` for 5 s, then sends `SIGCONT` and waits 2 s **after every step of
  the visitor run**, starting with the first; `SIGCONT` runs from a `finally`
  block. This starves the iGPU less, so the visitor's game stops dropping keys.
  The host is never paused.
- The no-progress guard counts only steps that actually sent a key (debounce
  waits and other waits do not count): six keys with the same
  `(screen, highlight)` end the run, as do 60 steps or the 600 s wall clock
  (`--timeout`), instead of guessing more keys.

Cost and latency: Mercury Decide (free) bills nothing and the free tier is rate
limited; the Luna Decisions API is input-only at $0.10 per 1M input tokens
(OpenAI pricing). Each step sends one 1280×720 PNG plus a short prompt and four
questions: about 1.2 s per request on Mercury, 1–3 s on Luna. A three-client run
is on the order of 40–60 requests, dominated by game loading rather than API
time.

## Verified on this machine

| what | evidence |
|---|---|
| iGPU Vulkan under headless X | `MESA_VK_DEVICE_SELECT=1002:13c0! vulkaninfo` → RADV `RAPHAEL_MENDOCINO`, Vulkan 1.4, Mesa 26.2.5; `vkcube` renders |
| second Steam client in its own root | `.steampid`/`.steampath` created inside `$RIG/home-a`; logs `Logged On … U:1:<accountid-private-1>` |
| clone adopted without download | client log: `Loaded 7 apps`, `AppID 485510 state changed : Fully Installed`, `scheduler finished … No Error` |
| mod loads in the rig | `logs-a/nioh3p.log`: `EOS_Lobby_CreateLobby hooked through the import table (14119b1e0 …)`, callbacks for `CCallback<CSessionP2pSteam, LobbyDataUpdate_t>` |
| Nioh renders on the iGPU | gamescope selected RADV `RAPHAEL_MENDOCINO`; compositor screenshot `shots/a-nioh-composited.png` shows the opening movie, and `shots/a-startup-confirmed.png` shows interactive brightness setup |

## Gotchas (each one cost a debugging session)

| symptom | cause | fix |
|---|---|---|
| client starts, then segfaults; `invalid name/address:port string provided to BSetIpPortFromName` | `-master_ipc_name_override` is parsed as `host:port` on Linux, not a name | don't pass it; a separate `HOME` is enough isolation |
| external library ignored, `Loaded 0 apps` | entry `"0"` in `libraryfolders.vdf` is the root library and its path is not honoured; other entries need a `steamapps/libraryfolder.vdf` marker, and Steam rewrites the file on startup | clone the game into the root library |
| Proton refuses to start: `FileNotFoundError: …/compatdata/485510/pfx.lock` | Proton creates the lock inside `compatdata/<appid>`, which must exist | `mkdir -p` it before launching |
| direct launch disappears and Steam UI remains | a process trace is needed to identify the game exit; the UI alone is not evidence of loading | enable `WINEDEBUG=+timestamp,+pid,+tid,+process` with `PROTON_LOG=1`; inspect the game PID's create/terminate events |
| `Presenter: ... VK_ERROR_SURFACE_LOST_KHR` on Xvfb | the game cannot present to that surface | keep Steam on Xvfb, run the game under headless gamescope |
| `Failed to initialize WSI` / DXGI factory creation failure | invalid `DXVK_WSI_DRIVER=xcb` override | remove it; the old recommendation was wrong |
| game boots, finds no data (`Boot Path:` wrong) | Nioh resolves data relative to the working directory | `cd` to the game directory first |
| no video settings, first run on a fresh prefix | `config.xml` is written by the launcher, and `nioh.exe` reads it | `setup.sh` seeds a 720p windowed low-detail one |
| `Proton: Error while copying … amdxcffx64.dll` | Proton 11.0's own tool prefix fails to build from a clone | pin `proton_experimental` and drop the broken tool clones |
| Steam UI window never maps (black display) | hand-editing `config/config.vdf` with a bad regex | restore it from the original (`cp --reflink=auto`; COW protects the source) |
| client loses window when the rig client is killed | the account picker re-appears on every start | `AutoLoginUser` + `MostRecent` pins in `setup.sh` |

## Boundaries

- Authenticated Steam tests require distinct licensed Steam accounts. The LAN
  experiment instead uses separate local emulator identities and prefixes;
  that does not demonstrate three authenticated Steam accounts playing online.
  The owned game executable is unchanged; no DRM-bypass patch is applied.
- Every authenticated Steam account used by the rig must be **Invisible**.
  Verify the label in its private-display Friends window before using it;
  `-silent` only hides the client and does not set presence. The active
  cloned client (`home-b`, display `:10`, account <account-name-private-3>) was visually verified
  Invisible in `shots/steam-b-invisible-verified.png`.
- Removals inside the rig go through `trash`/`trash-put`, never `rm`.

## Direct LAN launch status (2026-10-20)

The separate `lan-a`, `lan-b` and `lan-c` directories are experimental emulator
game copies; `setup.sh` manages the Steam-library copies shown above, not these
directories. Slots a and b reach gameplay with the updated proxy.
A cold direct launch first requested `steam://run/485510//` and exited with code 53;
leaving the cloned Linux Steam client running, then using full `proton run`,
allowed subsequent game launches. Keep that client on its private Xvfb display
(`tools/rig/run.sh b client`, with `Xvfb :10` already running), separate from
the game's compositor.

Both LAN clients have joined Wreathed in Flame and rendered each other's
characters, with visitor movement visible on the host. With the visitor
allowance build, a third client joins the same mission: see `FINDINGS.md`,
"Three players in one mission".

Slot c uses its own `HOME`, runtime directory, `compat-c/pfx`, save account and
LAN identity, but reuses slot b's cloned Proton/client installation. Prepare
`home-c/.steam/sdk64` and `sdk32` as symlinks to that installation's `linux64`
and `linux32` directories. Without them, Proton's `steam.exe` reports
`steamclient_init unable to load native steamclient library` and asserts
`!status`; setting `STEAM_COMPAT_CLIENT_INSTALL_PATH` alone did not supply them.

The third LAN identity is SteamID `76561198023713187` (`Nioh-LAN-C`), with a
distinct Epic emulator ID ending in `03`. Its save account directory is
`63447459`. Save copies were transferred with the original
[Nioh Savedata Decryption Tool v1.3](https://github.com/pawREP/Nioh-Savedata-Decryption-Tool/releases/tag/1.3),
using `-i source/SAVEDATA.BIN -sid 63447459 -o SAVEDATA.BIN` for both the system
and character save. Run it with the destination save folder as its working
directory: the tool discards directory components from `-o`. Do not disable
checksum checks or change source saves.

The navigation harness discovers displays and compositor sockets from each
live `nioh.exe` environment; use `nioh_nav.py -s c shot name.png` for C.
Actual keyboard controls: **BackSpace** returns to the previous menu;
**Escape** opens the equipment/system overlay. At a shrine, hold **E** for
about 1.5 seconds to pray rather than tapping it.

Enable process tracing without changing Proton's files:

```sh
WINEDEBUG=+timestamp,+pid,+tid,+process \
  bash "$HOME/nioh3p-test/scripts/run-b.sh"
```

The launcher already sets `PROTON_LOG=1`; the resulting log is
`$RIG/home-b/steam-485510.log`. This Proton version preserves an explicit
`WINEDEBUG` value (`env.setdefault` in its logging setup).

Use `proton run` for the actual game. An intermediate `runinprefix` attempt
used WineD3D instead of the normal graphics setup and faulted; it is not an
equivalent game-launch shortcut. The navigation harness settles the UI before
requesting a screenshot and waits for the PNG's complete IEND trailer. Sleeping
after requesting a screenshot cannot change the frame already captured.

If the title animates but ignores XTest input, inspect the compositor output:
one stale slot-b session logged `Unhandled event EI_EVENT_SYNC (91)` and
`Unhandled libei event!`. A fresh compositor with the native client on Xvfb
accepted input. SIGTERM left some old compositors alive on this machine;
confirm termination before launching another one, and verify both the
Xwayland display and `gamescope-N` socket rather than assuming their numbers.

## Operational boundaries

- Never use a broad `pgrep -f … | kill` or `pkill -f …` compound command for
  cleanup. Its pattern can match the issuing shell's own command line and kill
  it before subsequent copying, logging or launch commands execute. Verify
  PID ownership using the rig HOME/prefix, then target those PIDs only.
- The Wine prefix and saves are descendants of the rig Steam root:
  `$RIG/home-b/.local/share/Steam/steamapps/compatdata/485510/pfx`.
  Trashing the root also trashes its prefix. A reflink is not a separate tree.
- `setup.sh` trashes the proxy and INI in the slot-b **Steam-library** copy to
  keep that copy stock. It does not periodically remove logs. Other reported
  file disappearances have no identified cause; an external cleaner is not
  established.
- An IAT hook's silence does not prove that `WinMain` returned or that no
  other module terminated the game. Match Wine's process IDs and exit events;
  a surviving launcher process is not proof that the game is alive.
- The Linux Steam process observed during this run belonged to slot b's
  cloned root. Neither a Steam-looking screen nor `xalia.exe` alone proves
  interaction with the user's real desktop Steam client.

## Checkpoint (2026-10-21, player package)

Three-player co-op was verified in this rig: host B plus visitors A and C in
one mission, each rendering the others, moving independently, and a visitor's
attack visible on the host. When the first visitor quit, host and second visitor
stayed in co-op. Evidence and remaining gaps are in `FINDINGS.md`
("Host visitor allowance", "Three players in one mission").

- Join order for a run: B prays at the starting shrine → Summon Visitor →
  Make Offering; then each visitor goes Starting Point → Torii Gate → Random
  Encounters → Search. The host has no Pray prompt while a visitor is present.
- **Single-iGPU limit.** With two clients already in the mission, the third
  renders at about 1 fps and its load takes ~5 minutes. The host may time its
  slot out meanwhile. To emulate separate machines, pause the *other visitor's*
  `nioh.exe` with `SIGSTOP` in 5 s bursts while the newcomer loads, sending
  `SIGCONT` after each burst and always at the end. Never pause the host. This
  cut a join to 33 s.
- Slots `lan-a`, `lan-b` and `lan-c` now run the contents of
  `dist/nioh3p-player.zip` unchanged: `dinput8.dll` (SHA-256
  `502c55081dc0336fd292486f990d6b6b1f0098a078dbbb752cde6190fa08cb9a`, the lean
  build; the earlier `b66d2958…` and `fd4a5824…` builds passed the same run) and the
  player `nioh3p.ini` (CRLF, `trace=0`, empty `out_dir`), so each slot's log is
  `lan-*/nioh3p.log` with the previous launch in `nioh3p.prev.log`. The
  research configs were kept as `lan-*/nioh3p.ini.rig-research`. With this build
  host B plus A and C joined one mission again. All three logged
  `mode: player (trace=0)` and
  `game visitor allowance: recruitment opens 2 visitor slot(s) (shrine setter 2)`,
  and the host logged the metadata guard when C joined (with the safety build:
  18:23:48, now installed by one `lock cmpxchg16b`). Over 2 minutes of
  three-player play the logs grew 0 bytes (3.6–3.7 KB per client per launch).
  The research build grew ~120 KB/s on the host. Safety-build evidence:
  `shots/safe-b-3p.png` (host sees both visitors), `shots/safe-c-after.png`
  (C in co-op after moving and attacking).
- `tools/rig/coop.py run --host b --visitors a c` took all three cold-booted
  slots into one mission with no manual input: 6 min 17 s, 72 Mercury Decide
  decisions (B 25, A 26, C 21 with A relieved), trace
  `shots/coop-20261021-194424.jsonl`, host view `shots/drv-final-b-3p.png`.
- If a visitor accidentally starts a solo mission from the map, leave it with
  Items → **Himorogi Fragment** → Use. That forfeits Amrita gained in the
  mission. On the world map, `BackSpace` jumps the cursor to **Starting Point**.
- Read-only rig probes in `$RIG/tools-tmp` (freestanding Win64; run each with
  `proton runinprefix` from the target slot's HOME/prefix environment):
  `admission-poll3.exe` (10 ms poller for session root, slots, allowance and
  loader phase), `peek.exe SIZE [*]ADDR[+OFF]`, `rip-sample.exe`, `memdump.exe`.
  Diagnostic writers: `poke.exe`, `budget-set.exe` (sets the host allowance to
  2; it is used to reopen recruitment between rejoin tests), and `codepatch.exe`
  (refuses to write unless the expected bytes match).
- Do not attach a Win32 debugger to an in-mission client: `DebugActiveProcess`
  made A exit silently. The old breakpoint/watchpoint probes stay only as
  reference sources.
- Shutdown this session: all three games quit through their native dialog
  (exit 0). Steam was visually verified **Invisible**
  (`shots/steam-b-invisible-stop5.png`) and shut down normally. Xvfb `:10` was
  stopped, and a final scan found no live process with a rig HOME. The 4.7 GB
  research-era `logs-*/nioh3p.log` files were trashed.

To resume: start `Xvfb :10`, run `tools/rig/run.sh b client`, verify Invisible
in the Friends window, then run `tools/rig/run.sh {b,a,c} lan`.
