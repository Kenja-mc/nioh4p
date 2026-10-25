# The headless test rig (`tools/rig/`)

Two extra Nioh instances run with no window on the desktop. They use no dGPU, and they write nothing anywhere in the user's Steam root, library or Proton prefix. The three-player tests need this setup. A host and a second player come up on demand while the machine stays usable.

```
$RIG = ~/nioh3p-test
├── home-a/.local/share/Steam/     slot a: own Steam root (COW clone of the client)
│     ├── steamapps/common/Nioh/   the game, with dinput8.dll + nioh3p.ini (mod)
│     ├── steamapps/common/Proton - Experimental
│     └── steamapps/compatdata/485510/pfx   own Wine prefix, own saves, own config
├── home-b/…                       slot b: same, stock game (no mod DLL)
├── lan-a/, lan-b/, lan-c/, lan-d/ separate LAN game copies, all with the proxy
├── compat-c/pfx                  third LAN prefix (not inside a Steam root)
├── compat-d/pfx                  fourth LAN prefix (not inside a Steam root)
├── home-c/                       third instance's private HOME
├── home-d/                       fourth instance's private HOME
├── logs-a/, logs-b/, logs-c/     research-era logs (out_dir); player ini logs to lan-*/
├── run-a/, run-b/, run-c/, run-d/ per-slot XDG_RUNTIME_DIR
└── shots/                         screenshots taken from the rig display
```

Steam's UI uses `Xvfb :9` for slot a and `:10` for slot b. The game must run inside headless gamescope instead, because Nioh's Vulkan presentation failed on Xvfb. DXVK's filter selects the iGPU for the game, and `--prefer-vk-device` selects it for the compositor. Each slot has a separate Steam root, game clone and prefix. btrfs reflinks keep rig writes separate from the original installation.

## Running it

`tools/rig/rig.py` runs the whole LAN rig. Run it from the repository root. The LAN copies use a Steam emulator, so no Steam client and no Steam account is needed for these steps.

```sh
tools/rig/rig.py up b a c d       # start the lan games in turn; each waits for the proxy's preflight line and a frame
tools/rig/rig.py status           # rig processes per slot (matched by rig HOME only)
tools/rig/rig.py state b a c d    # read-only game state through peek.exe: loader phase, Tokoyo mode, role, slot, sync counters, frame brightness
tools/rig/rig.py deploy research a b   # copy build/research/dinput8.dll (or "player": build/dinput8.dll) into lan-<slot>; refuses a running slot
tools/rig/rig.py ini a trace=1 dump_image=1   # set keys in lan-<slot>/nioh3p.ini, keeping its line endings
tools/rig/rig.py collect NAME     # copy every slot's logs, launch output and the newest coop trace to $RIG/runs/NAME
tools/rig/rig.py down             # stop all games (--client also stops the slot b Steam client)
```

`down` sends SIGTERM to `nioh.exe` first, then to the rest of the slot's Wine and compositor processes, then SIGKILL to what is left. It decides ownership by the `HOME` in `/proc/<pid>/environ`, so the user's own Steam can never match.

The lower-level commands below remain for the Steam-library slots and for input:

```sh
tools/rig/setup.sh            # build/refresh the clones (idempotent, fast)
tools/rig/login.sh a          # start slot a's client and pick its account tile
tools/rig/run.sh a client     # the same client start, without the tile click
tools/rig/run.sh a game       # request launch through the running Steam client
tools/rig/run.sh a lan        # prepared LAN game in headless gamescope
tools/rig/run.sh c lan        # compat-c + lan-c (d: compat-d + lan-d), reusing slot b's cloned Proton
tools/rig/run.sh a probe peek.exe 4 '*0x1418715e0+0x5d0'   # read-only probe from $RIG/tools-tmp in the slot's prefix
tools/rig/shot.sh a name      # screenshot slot a -> $RIG/shots/name.png
DISPLAY=:9 python3 tools/rig/xkey.py key Return Down space
DISPLAY=:9 python3 tools/rig/xkey.py move 535 384 && … click 1
```

Start the client before `run.sh … game`. The raw Steam binary forwards a request to an existing client, but it cannot bootstrap its runtime libraries alone. A cold raw-binary launch failed here with missing `libXtst.so.6`. `… client` uses `steam.sh` to establish that environment.

In each rig client's Nioh Properties, select Proton Experimental. Set Launch Options to the command below, and adjust the repository path if necessary.

```sh
NIOH3P_HEADLESS=1 NIOH3P_SCOPE=0 NIOH3P_GPU=RAPHAEL_MENDOCINO NIOH3P_VK_DEVICE=1002:13c0 /home/<you>/Projects/nioh3p/tools/quiet-launch.sh %command%
```

The wrapper clears inherited display and WSI overrides before it starts gamescope. The isolated runtime has no systemd user bus, hence `NIOH3P_SCOPE=0`. This value disables the CPU and RAM scope limits of the wrapper. Keep Steam Cloud disabled in the test roots. For a diagnostic run, prepend `PROTON_LOG=1`. The log is in that slot's HOME.

Gamescope chooses its own Xwayland display. Read `Starting Xwayland on :N` in its output, then use `DISPLAY=:N` for game input. Xvfb screenshots capture Steam, not the game. Capture the actual frame of the compositor instead:

```sh
XDG_RUNTIME_DIR="$HOME/nioh3p-test/run-a" GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
  gamescopectl screenshot "$HOME/nioh3p-test/shots/game-a.png"
```

Use the Wayland display that gamescope prints if it is not `gamescope-0`. Xwayland's XTest pointer input here uses client-window coordinates, not the offset of the window on the root. The input helper holds keys across game frames. Nioh's polled input missed an instantaneous down/up.

References: [gamescope's Steam launch examples](https://github.com/ValveSoftware/gamescope#examples)
and [Proton runtime options](https://github.com/ValveSoftware/Proton#runtime-config-options).

The Steam controller notice and the Nioh launcher can both require UI confirmation before the game starts. A forwarded request, a process count or a nonblack Steam screenshot is not proof of a running game.

Stored authentication sometimes survives cloning and a restart, and sometimes it does not. Both accounts required QR authentication in the first run. A QR authorizes the account selected in the mobile app. A pre-filled desktop username does not bind the QR to it.

## Decision-model driver

`tools/rig/coop.py` replaces OCR menu matching with a loop of screenshot, decision model and deterministic key table. The model only reports what it sees: `screen`, `highlight`, and the predicates `without_conditions` and `coop_partner`. A per-state table sends keys only from the verified recipes. An unclear frame cannot turn into an improvised keypress.

The tables follow the Torii Gate companion flow: Starting Point, Torii Gate, Yokai Realm with a Companion, Without Conditions, Search. The host and the visitors use the same table. The first client to search finds no lobby and creates one, so it becomes the host. Every later client finds that lobby. No Ochoko Cup is used. A third table, `leave`, takes a client out of a mission with the Himorogi Fragment. That item stays in the inventory, but the character loses the Amrita that it carries.

The engine is a reusable harness. Perception lives in `tools/rig/perceive.py`, execution in `tools/rig/drive.py`, and all Nioh choices and rules in `tools/rig/profiles/nioh-coop.json`. `coop.py` is the thin Nioh front-end: the CLI plus a few named hooks. To drive another game or window manager, add a profile instead of editing the loop. See [`docs/RIG_HARNESS.md`](RIG_HARNESS.md) for the question-spec format, the `CommandCapture`/`CommandActuator` route, and confidence guidance.

Two backends exist. Choose one with `--backend {mercury,luna}` or `COOP_BACKEND`. The default is `luna`. The OpenRouter key for `mercury` is revoked.

- `mercury`: OpenRouter Mercury Decide (`inception/mercury-decide:free`) at `https://openrouter.ai/api/alpha/decisions`. It has a free tier and is rate limited. It sends choices as `criteria` maps and predicates as `noul` questions.
- `luna`: Experiential Labs Decisions (`gpt-6-luna-decisions`). It uses named questions and choices, and `probability` answers.

`--model` overrides the model id of the backend. Both responses are normalized to the same decision dict. The JSONL trace records `backend`, `model` and the per-request `usage`: `input_tokens`, `output_tokens`, `cost`.

Environment variables:

- `OPENROUTER_API_KEY`, or `OPENROUTER_ENV_FILE` naming a file that holds an `OPENROUTER_API_KEY=` line. This variable belongs to backend `mercury`.
- `EXPLABS_API_KEY`, or `EXPLABS_KEY_FILE` naming a file that holds it. This variable belongs to backend `luna`.

Keys are never logged or printed. The driver sends one request per step. The model gets a quality-85 JPEG of the frame, not the 1.6 MB PNG from gamescope. On ten rig frames the answers matched, the upload was 1/15 of the size, and the median Luna latency fell from 2.3 s to 0.9 s. The driver re-shoots once if a low-confidence answer could change the action: the `screen` itself, or the `highlight` on a screen whose rules test the highlight. A low highlight on a waiting screen (searching, preparation, in mission) needs no second frame. In one three-client run this cut 65 of 119 steps from two requests to one. If the choice is still below 0.6, `screen` degrades to `other` (wait) and `highlight` degrades to `uncertain`. No Return rule matches then, so only safe moves and waits fire. The trace keeps the raw choice and confidence. Every step appends one JSON line to `$RIG/shots/coop-<timestamp>.jsonl` with the screenshot, answers, action and latency, and prints a one-line summary.

Accuracy on rig screenshots: Mercury Decide classified 7/7 screens and 6/6 highlighted items. It rejected 3/3 mission-node negatives, so it never mistook a mission node for STARTING POINT. Luna drove the whole companion flow on 2026-10-25 for two, three and four clients, from a cold boot to a joined mission.

A model-free classifier was measured against the 717 labeled frames of the earlier traces and rejected. Full-frame OCR (tesseract, 0.3 s) read the hint boxes but missed most screens without text, and a frame-hash cache matched only 13 % of frames at a threshold with no wrong labels, because the menu backgrounds animate. The rule tables stay deterministic. Only perception uses the model. An agent without a model can still drive one step at a time with `decide` and the `rig.py state` fields.

```sh
export EXPLABS_KEY_FILE=~/.omp/agent/provider-keys/experiential-labs
python3 tools/rig/coop.py host b --backend luna            # B searches first and owns the lobby
python3 tools/rig/coop.py visit a --backend luna           # A searches and joins B
python3 tools/rig/coop.py run --host b --visitors a c --backend luna   # B, then A and C together
python3 tools/rig/coop.py leave b --backend luna           # back to the Torii Gate (costs Amrita)
python3 tools/rig/coop.py decide a --role visitor --backend luna   # one frame, no keys sent
python3 tools/rig/coop.py host b --dry-run --backend luna  # perceive + print action only
```

`decide` captures one frame. It prints the perception and the action that the guard or table selects as JSON. It exits 0 without sending anything.

Safety rules:

- On the title menu, the driver sends `Return` only when `highlight` is `continue` with confidence ≥ 0.9. `new_game` gets `Down` and `load_or_system` gets `Up`. Anything else, including `uncertain`, waits 1.5 s. This rule keeps a run from starting a new game.
- On `world_map`, the driver sends `Return` only when `highlight` is `starting_point_node` with confidence ≥ 0.9. Every other world-map frame gets `BackSpace`. The footer binding `B: STARTING POINT` jumps the cursor onto the starting point instead of cycling node by node. This rule prevents a client from starting a solo mission.
- On the companion panel, the driver first sets the top row to `Without Conditions`. It sends `Right` only while that row is selected, so a press cannot change the Friends or Secret Word rows. It sends `Return` only on the Search button.
- The driver accepts a `dialog` only when `highlight` is `confirm_button`. It cancels every other dialog frame with `BackSpace`. The `leave` table also accepts YES, because the Himorogi Fragment asks for it.
- A visitor that shows `in_mission` without a new `Steam JoinLobby:` log line is reported, and the run stops. There is no automatic recovery from a solo mission.
- Once the `Steam JoinLobby:` line of a visitor exists, the driver sends no keys at all. Every screen waits 3 s until success or timeout.
- The host succeeds when a new `Steam_ISteamMatchmaking::CreateLobby` line appears. From then on it sends no keys, because `Return` on the searching popup would cancel the search. The driver reads the log in the `out_dir` of the slot INI, or in the game folder when `out_dir` is empty.
- After any `Return`, an unchanged `(screen, highlight)` gets a 1.5 s wait instead of another `Return`. The driver resends the key only after 3 unchanged perceptions, because the game can drop it.
- `run` starts the host first. When the host owns its lobby, all visitors run at the same time, because the lobby takes visitors only until its preparation countdown ends.
- `--relieve` (on `visit` only) pauses the `nioh.exe` of another visitor (matched by rig HOME) with `SIGSTOP` for 5 s. It then sends `SIGCONT` and waits 2 s after every step. `SIGCONT` runs from a `finally` block. The host is never paused.
- The no-progress guard counts only steps that sent a key. Six keys with the same `(screen, highlight)` end the run. 150 steps or the 600 s wall clock (`--timeout`) also end it, instead of guessing more keys.

Cost and latency: the Luna API reported `cost` 0.0 for every request in the 2026-10-25 traces, at about 2,300 input tokens per frame. A request takes about 1 s. A three-client Torii run from a cold boot to the mission takes about 5 minutes and 150 requests. Game loading takes more time than the API.

## Verified on this machine

The table below gives each test and the evidence that it passed. Each row stands alone. The logs and screenshots in `$RIG` hold the original output.

| what | evidence |
|---|---|
| iGPU Vulkan under headless X | `MESA_VK_DEVICE_SELECT=1002:13c0! vulkaninfo` → RADV `RAPHAEL_MENDOCINO`, Vulkan 1.4, Mesa 26.2.5; `vkcube` renders |
| second Steam client in its own root | `.steampid`/`.steampath` created inside `$RIG/home-a`; logs `Logged On … U:1:<accountid-a>` |
| clone adopted without download | client log: `Loaded 7 apps`, `AppID 485510 state changed : Fully Installed`, `scheduler finished … No Error` |
| mod loads in the rig | `logs-a/nioh3p.log`: `EOS_Lobby_CreateLobby hooked through the import table (14119b1e0 …)`, callbacks for `CCallback<CSessionP2pSteam, LobbyDataUpdate_t>` |
| Nioh renders on the iGPU | gamescope selected RADV `RAPHAEL_MENDOCINO`; compositor screenshot `shots/a-nioh-composited.png` shows the opening movie, and `shots/a-startup-confirmed.png` shows interactive brightness setup |

## Gotchas (each one cost a debugging session)

The table below lists faults that each cost a debugging session. Each row gives the symptom, the cause and the fix. Some symptom text is quoted from tool output.

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

- Authenticated Steam tests require distinct licensed Steam accounts. The LAN experiment instead uses separate local emulator identities and prefixes. That does not demonstrate three authenticated Steam accounts playing online. The owned game executable is unchanged, and no DRM-bypass patch is applied.
- Every authenticated Steam account that the rig uses must be Invisible. Make sure that the label shows in its private-display Friends window before you use the account. `-silent` only hides the client and does not set presence. The active cloned client (`home-b`, display `:10`, account PlayerTwo) showed Invisible in `shots/steam-b-invisible-verified.png`.
- Removals inside the rig go through `trash` or `trash-put`, never `rm`.

## Direct LAN launch status (2026-10-20)

The separate `lan-a`, `lan-b` and `lan-c` directories are experimental emulator game copies. `setup.sh` manages the Steam-library copies shown above, not these directories. Slots a and b reach gameplay with the updated proxy. A cold direct launch first requested `steam://run/485510//` and exited with code 53. Leaving the cloned Linux Steam client running allowed later game launches with full `proton run`. Keep that client on its private Xvfb display (`tools/rig/run.sh b client`, with `Xvfb :10` already running), separate from the compositor of the game.

Both LAN clients joined Wreathed in Flame and rendered the characters of each other. Visitor movement was visible on the host. With the visitor allowance build, a third client joins the same mission. See `FINDINGS.md`, "Three players in one mission".

Slot c uses its own `HOME`, runtime directory, `compat-c/pfx`, save account and LAN identity. It reuses the cloned Proton and client installation of slot b. Prepare `home-c/.steam/sdk64` and `sdk32` as symlinks to the `linux64` and `linux32` directories of that installation. Without them, Proton's `steam.exe` reports `steamclient_init unable to load native steamclient library` and asserts `!status`. Setting `STEAM_COMPAT_CLIENT_INSTALL_PATH` alone did not supply them.

The third LAN identity is SteamID `<steamid-lan-c>` (`Nioh-LAN-C`), with a distinct Epic emulator ID ending in `03`. Its save account directory is `<accountid-c>`. The save transfer used the original [Nioh Savedata Decryption Tool v1.3](https://github.com/pawREP/Nioh-Savedata-Decryption-Tool/releases/tag/1.3). The command was `-i source/SAVEDATA.BIN -sid <accountid-c> -o SAVEDATA.BIN`, for both the system and character save. Run the tool with the destination save folder as its working directory. The tool discards directory components from `-o`. Do not disable checksum checks and do not change source saves.

The fourth LAN identity is SteamID `<steamid-lan-d>` (`Nioh-LAN-D`), with a distinct Epic emulator ID ending in `04` (epicid `00000000000000000000000000000004`, productuserid `<productuserid-lan-d>`). Its save account directory is `<accountid-d>`. The same tool converted slot c's `<accountid-c>` saves for slot d. Run it as `-i <c file> -sid <accountid-d> -o SAVEDATA.BIN`. Use the destination save folder as the working directory. The tool writes the output and then waits on stdin. Give it Enter or stop it once the file exists. While the tool runs, Proton also starts its Xalia helper process next to the game.

The navigation harness discovers displays and compositor sockets from the environment of each live `nioh.exe`. Use `nioh_nav.py -s <slot> shot name.png`; the harness lists a, b, c and d. Actual keyboard controls: BackSpace returns to the previous menu. Escape opens the equipment and system overlay. At a shrine, hold E for about 1.5 seconds to pray rather than tapping it.

Enable process tracing without changing the files of Proton:

```sh
WINEDEBUG=+timestamp,+pid,+tid,+process \
  bash "$HOME/nioh3p-test/scripts/run-b.sh"
```

The launcher already sets `PROTON_LOG=1`. The resulting log is `$RIG/home-b/steam-485510.log`. This Proton version preserves an explicit `WINEDEBUG` value (`env.setdefault` in its logging setup).

Use `proton run` for the actual game. An intermediate `runinprefix` attempt used WineD3D instead of the normal graphics setup and faulted. It is not an equivalent game-launch shortcut. The navigation harness settles the UI before it requests a screenshot, and it waits for the complete IEND trailer of the PNG. Sleeping after a screenshot request cannot change the frame that was already captured.

If the title animates but ignores XTest input, inspect the compositor output. One stale slot-b session logged `Unhandled event EI_EVENT_SYNC (91)` and `Unhandled libei event!`. A fresh compositor with the native client on Xvfb accepted input. SIGTERM left some old compositors alive on this machine. Make sure that a compositor ended before you launch another one. Also make sure that the Xwayland display and the `gamescope-N` socket are the expected ones, instead of assuming their numbers.

## Windows 11 VM (Defender test)

A Windows 11 Pro VM runs in Docker with `dockurr/windows`. The container `nioh3p-winvm` uses the local image `nioh3p/windows:6.06-trustfix`, built from `~/nioh3p-test/winvm/image-trustfix/Dockerfile`. The stopped container `nioh3p-winvm-6.06-orig` is the rollback copy. Do not start both, because they share the VM disk. Its disk, OEM script and SSH key live in `~/nioh3p-test/winvm`. No sudo and no Steam account are needed. The OEM script enables OpenSSH, so you can run commands with the key in that folder.

```bash
docker start nioh3p-winvm                       # boot the VM; the screen is at http://127.0.0.1:8006/
ssh -i ~/nioh3p-test/winvm/key -p 2222 Docker@127.0.0.1 'Get-MpComputerStatus'   # PowerShell in the VM
docker stop -t 120 nioh3p-winvm                 # shut it down cleanly
```

`~/nioh3p-test/winvm/shared` is `\\host.lan\Data` in the VM. The rig copy `lan-a` is mounted read-only under it as `nioh-lan-a`. Programs that need the desktop (for example `nioh.exe`) run through the scheduled task `nioh3p-run`, which starts `C:\Games\run-nioh.bat` in the signed-in session. The VM already has the Visual C++ runtime and the DirectX June 2010 runtime. The game itself stops with code 53 in the VM because no Steam client runs there. Results are in `FINDINGS.md`, "Windows 11 VM test".

## Operational boundaries

- Never use a broad `pgrep -f … | kill` or `pkill -f …` compound command for cleanup. Its pattern can match the command line of the issuing shell, and it then kills that shell before later copying, logging or launch commands run. Make sure that you own each PID by its rig HOME or prefix, then target those PIDs only.
- The Wine prefix and saves are descendants of the rig Steam root: `$RIG/home-b/.local/share/Steam/steamapps/compatdata/485510/pfx`. Trashing the root also trashes its prefix. A reflink is not a separate tree.
- `setup.sh` trashes the proxy and INI in the slot-b Steam-library copy to keep that copy stock. It does not periodically remove logs. Other reported file disappearances have no identified cause. An external cleaner is not established.
- Silence from an IAT hook does not prove that `WinMain` returned, and it does not prove that no other module terminated the game. Match the process IDs and exit events of Wine. A surviving launcher process is not proof that the game is alive.
- The Linux Steam process observed during this run belonged to the cloned root of slot b. A Steam-looking screen alone does not prove interaction with the real desktop Steam client of the user. Neither does `xalia.exe` alone.

## Checkpoint (2026-10-21, player package)

This rig ran three-player co-op: host B plus visitors A and C in one mission. Each client rendered the others and moved independently, and an attack by a visitor was visible on the host. When the first visitor quit, the host and the second visitor stayed in co-op. Evidence and remaining gaps are in `FINDINGS.md` ("Host visitor allowance", "Three players in one mission").

- Join order for a run: B prays at the starting shrine, then Summon Visitor, then Make Offering. Each visitor then goes Starting Point, Torii Gate, Random Encounters, Search. The host has no Pray prompt while a visitor is present.
- Single-iGPU limit: with two clients already in the mission, the third renders at about 1 fps and its load takes about 5 minutes. The host can time its slot out meanwhile. To emulate separate machines, pause the `nioh.exe` of the other visitor with `SIGSTOP` in 5 s bursts while the newcomer loads. Send `SIGCONT` after each burst and always at the end. Never pause the host. This cut a join to 33 s.
- Slots `lan-a`, `lan-b` and `lan-c` now run the contents of `dist/nioh3p-player.zip` unchanged: `dinput8.dll` and the player `nioh3p.ini`. `dinput8.dll` is the lean build with SHA-256 `502c55081dc0336fd292486f990d6b6b1f0098a078dbbb752cde6190fa08cb9a`. The earlier `b66d2958…` and `fd4a5824…` builds passed the same run. The player `nioh3p.ini` is CRLF with `trace=0` and an empty `out_dir`. Each slot's log is therefore `lan-*/nioh3p.log`, with the previous launch in `nioh3p.prev.log`. The research configurations were kept as `lan-*/nioh3p.ini.rig-research`. With this build, host B plus A and C joined one mission again. All three logged `mode: player (trace=0)` and `game visitor allowance: recruitment opens 2 visitor slot(s) (shrine setter 2)`. The host logged the metadata guard when C joined. With the safety build that guard ran at 18:23:48, and one `lock cmpxchg16b` now installs it. Over 2 minutes of three-player play the logs grew 0 bytes (3.6–3.7 KB per client per launch). The research build grew about 120 KB/s on the host. Safety-build evidence: `shots/safe-b-3p.png` (host sees both visitors) and `shots/safe-c-after.png` (C in co-op after moving and attacking).
- Stock-filter run (build `1003e4e7…`, `drop_lock_filter=0 drop_slots_filter=0`): B, A and C in one mission. Both joins used the game's own search filters.
- Player/research split (build `48415ff0…`, player DLL 38,400 bytes, write window RWX then RX): B, A and C in one mission again. Both visitors joined (JoinLobby 23:35 and 23:39), the metadata guard installed at the second join, and each log stayed under 5 KB. Shots: `shots/split-3p.png`, `shots/split-*-move.png`.
- `tools/rig/coop.py run --host b --visitors a c` took all three cold-booted slots into one mission with no manual input. It took 6 min 17 s and 72 Mercury Decide decisions (B 25, A 26, C 21 with A relieved). Trace: `shots/coop-20261021-194424.jsonl`. Host view: `shots/drv-final-b-3p.png`.
- If a visitor accidentally starts a solo mission from the map, leave it with Items, Himorogi Fragment, Use. That forfeits Amrita gained in the mission. On the world map, `BackSpace` jumps the cursor to Starting Point.
- Read-only rig probes live in `$RIG/tools-tmp` (freestanding Win64). Run each with `proton runinprefix` from the HOME and prefix environment of the target slot. `admission-poll3.exe` is a 10 ms poller for session root, slots, allowance and loader phase. `peek.exe SIZE [*]ADDR[+OFF]`, `rip-sample.exe` and `memdump.exe` are the other probes. Diagnostic writers: `poke.exe`, `budget-set.exe` and `codepatch.exe`. `budget-set.exe` sets the host allowance to 2 and is used to reopen recruitment between rejoin tests. `codepatch.exe` refuses to write unless the expected bytes match.
- Do not attach a Win32 debugger to an in-mission client: `DebugActiveProcess` made A exit silently. The old breakpoint and watchpoint probes stay only as reference sources.
- Shutdown this session: all three games quit through their native dialog (exit 0). Steam showed Invisible (`shots/steam-b-invisible-stop5.png`) and shut down normally. Xvfb `:10` stopped, and a final scan found no live process with a rig HOME. The 4.7 GB research-era `logs-*/nioh3p.log` files went to trash.

To resume: start `Xvfb :10`, run `tools/rig/run.sh b client`, make sure that the Friends window shows Invisible, then run `tools/rig/run.sh {b,a,c} lan`.

## Checkpoint (2026-10-23, four players)

This rig ran four-player co-op: host B plus visitors A, C and D in one mission. The branch is `four-player`. Its player build has SHA-256 prefix `6db5351d` and 38,400 bytes, and `./build.sh` produced it. The proxy on this branch also raises the shrine setter cap. It patches the byte at RVA `0xd5fb4c`, the immediate of the cmp at `0xd5fb4a` (`cmp $0x3,%ebx`, bytes `83 fb 03`), to 4 when the allowance is 3, so the online shrine site can now pass the value 3 past the setter's cap check. With allowance 2 the game bytes stay stock.

- Smoke harness: it passed with `max_players` 3 and 4, in player and research mode, plus the ownership harness at both counts. The harness now reads `max_players` from `nioh3p.ini` to build its expectation. `NIOH3P_MAX_PLAYERS` appends an overriding `max_players` key in `tools/smoketest.sh`.
- Three-player regression with the branch build: host b plus visitors a and c, driven by `coop.py run --host b --visitors a c`. All three roles passed. The host logged `game create request: raising max members 2 -> 3 before allocation`. Every client logged `game visitor allowance: recruitment opens 2 visitor slot(s) (shrine setter 2)`. Host shot `shots/4p-reg-3p-b.png` shows three player characters.
- Four-player run: all four lan slots ran `max_players=4`. Each lan slot keeps its three-player ini as `lan-*/nioh3p.ini.3p`. Every client logged `game visitor allowance: recruitment opens 3 visitor slot(s) (shrine setter 3)`, so the setter cap raise installed on all four. Host b logged `game create request: raising max members 2 -> 4 before allocation` at 13:44:01.
- Join order: a, c and then d, driven by `coop.py run --host b --visitors a c d`. The driver relief (SIGSTOP pauses of the waiting visitors) paused a and c while d loaded. a and c passed the driver checks. d searched, found the lobby and sent JoinLobby at 13:56:28, with three JoinLobby lines in its log. The driver then hit its default 600 s wall timeout while d was still loading. d finished loading on its own and entered the host mission right after the driver timed out.
- Evidence shots: `shots/4p-mission-b.png` and `shots/4p-move3-b.png` show the host view with the host and two visible visitors; the third visitor is off camera (the driver trace records all four). Visitor a view with the host and the two other visitors in `shots/4p-hostmove-a.png`. Movement was shown both ways. After a, c and d each walked, `4p-move3-b.png` is the before frame and `4p-move-b.png` the after frame; the visitor positions differ between them. After the host walked, the visitor shots `4p-hostmove-a.png`, `4p-hostmove-c.png` and `4p-hostmove-d.png` were taken. Driver trace: `shots/coop-20261023-134257.jsonl`, 104 steps.
- Rig script changes: `tools/rig/run.sh` accepts slot d for lan (compat-d prefix, slot b client files). `tools/rig/nioh_nav.py` lists a, b, c and d. `tools/rig/coop.py` accepts two or three visitors, and `--relieve` now takes several slots. `tools/rig/setup.sh` writes `max_players=4` into slot a's ini on this branch.
- Remaining gaps at four: the party HUD still names only one partner. The coop driver default timeout of 600 s is too short for a fourth client's load. Pass `--timeout 900` for four-player runs. Mission finish, deaths, revives, rewards and a new offering after a visitor leaves are untested at four. d's join needed the relief pauses on a and c.
