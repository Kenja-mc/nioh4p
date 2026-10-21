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
  `fd4a58249fdadb208e1f10124cb786f8108f207876d278089e3f9b5dffe2df1c`, the
  Windows-safety build; the earlier `b66d2958…` build passed the same run) and the
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
- If a visitor accidentally starts a solo mission from the map, leave it with
  Items → **Himorogi Fragment** → Use. That forfeits Amrita gained in the
  mission. Then press Right on the map until **Starting Point** is selected.
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
  (`shots/steam-b-invisible-stop4.png`) and shut down normally. Xvfb `:10` was
  stopped, and a final scan found no live process with a rig HOME. The 4.7 GB
  research-era `logs-*/nioh3p.log` files were trashed.

To resume: start `Xvfb :10`, run `tools/rig/run.sh b client`, verify Invisible
in the Friends window, then run `tools/rig/run.sh {b,a,c} lan`.
