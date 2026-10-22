# nioh3p - three-player Nioh research mod

Working notes, instrumentation and the currently reachable part of a "3 players in
Nioh: Complete Edition co-op" mod, for the Steam build (appid 485510) running
under Proton on this machine.

**Status: three-player co-op works in the isolated LAN rig.** A host and two
visitors, all running this proxy, played one mission together: each rendered the
other two, each moved independently, and an attack by one visitor was visible
to the host (`docs/FINDINGS.md`, "Three players in one mission"). Authenticated
online play with three separate Steam accounts, and stock (unmodded) visitors,
have not been tested.

## What is here

| Path | What it is |
|---|---|
| `src/proxy/nioh3p_proxy.c` | the mod: a `dinput8.dll` proxy for Windows x64, freestanding (KERNEL32 only) |
| `src/proxy/dinput8.def` | exports, so the real DirectInput keeps working |
| `build.sh` | builds `build/dinput8.dll` (zig or mingw-w64) |
| `install.sh` / `install.sh uninstall` | puts two files in the game dir / removes them |
| `nioh3p.ini` | config: `max_players`, dump, hooks |
| `tools/pe.py`, `tools/analyze.py` | PE reader + `.pdata`-driven disassembly used for the analysis |
| `tools/rtti.py` | RTTI -> vtables -> methods (needs a runtime image dump) |
| `tools/lnk.py` | reads the `K300` archive containers: `list`, `info`, `scan`, `dump` |
| `tools/symtab.py` | symbol names out of the readable sections (`stats`, `find`, `dump`) |
| `tools/mergedump.py` | merges image snapshots for maximum decrypted coverage, with a per-section report |
| `tools/testgame/` | harness that imports `dinput8.dll` and EOSSDK exactly like the game does |
| `tools/smoketest.sh` | runs the harness or a copy of `nioh.exe` under Proton in a throwaway prefix |
| `tools/quiet.sh` | runs the smoke tests *without disturbing the desktop* (idle gate, offscreen display, capped cgroup) |
| `tools/quiet-launch.sh` | Steam launch-option wrapper: normal play as before, headless gamescope when a flag file is armed |
| `tools/package.sh` | builds `dist/nioh3p-player.zip` (DLL, player ini, player README) |
| `tools/rig/coop.py` | Nioh front-end for the rig harness: CLI, named hooks, `host`/`visit`/`run`/`decide`; see `docs/RIG.md` |
| `tools/rig/perceive.py`, `tools/rig/drive.py`, `tools/rig/profiles/` | reusable screenshot → decision model → key-table harness; add a profile to drive another game/UI; see `docs/RIG_HARNESS.md` |
| `docs/RIG.md` | the headless three-instance test rig |
| `docs/RIG_HARNESS.md` | perception/driver/profile architecture, question specs, `decide` smoke check |
| `docs/NAV_OPTIONS.md` | ranked options for faster rig navigation (memory oracle, save templates, vision) |
| `docs/FINDINGS.md` | evidence, co-op limits, what a real 3-player mod needs |

## What the mod does

1. **Forwards DirectInput** to the system `dinput8.dll` (it is loaded purely as a
   proxy; `nioh.exe` imports `dinput8.dll`, so it loads before the game's entry
   point runs). The launcher in the same folder also imports that DLL: the
   proxy stays inert there (no config, no log, no hooks).
2. **Dumps the decrypted runtime image** (research, `dump_image=1`; off in the
   player package). `nioh.exe`'s `.text` is encrypted at
   rest (entropy 7.997) and is only decrypted by its entry stub at runtime, so
   the game's code cannot be analysed statically. The proxy waits for the
   decryption and writes `nioh3p_image_boot.bin` / `..._settled.bin` (50.8 MB,
   raw image at `0x140000000`, with a `.json` manifest). That is the input any
   further netcode work needs.
3. **Raises the EOS lobby capacity.** It patches the game's import-table slot for
   `EOS_Lobby_CreateLobby` and raises `MaxLobbyMembers` to `max_players`, plus
   `EOS_LobbyModification_SetMaxMembers` if the game happens to use it. Each
   raise is logged with its original value, and the patch is re-applied if the
   game restores the slot. The Steam `CreateLobby`/`SetLobbyMemberLimit` path
   is raised the same way.
4. **Instruments the whole lobby conversation** (research, `trace=1` only),
   which is what turns a play session into reverse-engineering data:
   * `EOS_Lobby_CreateLobby` / `EOS_Lobby_JoinLobby` - arguments plus a **stack
     backtrace in module-relative form** (`nioh.exe+0x...`), so the game-side
     caller can be looked up directly in the dumped image.
   * `EOS_LobbyDetails_GetMemberCount` - the member count the game actually sees.
   * `EOS_LobbyDetails_CopyInfo` - the lobby's `MaxMembers`/`AvailableSlots` as a
     *joining* client sees them (this is the field that decides whether a stock
     client will accept a three-member lobby).
   * `EOS_LobbyModification_AddAttribute` - the matchmaking attribute keys and
     values, i.e. how the game labels its modes.
   * `EOS_LobbyModification_SetMaxMembers` - logged when present.

5. **Opens the game's own session to a third member** (supported executable
   only; every patch checks exact original bytes first):
   * raises the host's `CSessionP2pOpenCreateParam` member count before the
     native member arrays are allocated;
   * keeps the lobby compatibility metadata intact when the publisher assigns a
     descriptor to itself;
   * changes the host's visitor allowance — the "one visitor per offering"
     constant stored when recruitment opens — to `max_players - 1`, so the
     host's admission check accepts a second visitor instead of replying with a
     rejection.

## The data path and the symbols

Both are readable without running the game, which is where most of the recent
work went (details in `docs/FINDINGS.md` 6.2/6.3):

* 20 `K300` containers, 13,382 entries, 71.19 GB, every entry validated:
  `tools/lnk.py list`. Scripts (archives 06/08/15/18) carry the mission logic and
  the 1,072-name engine API the scripts may call; textures (`GT1G`, `KSLT`,
  `KTSR`), models (`MDLRESPK`), effects, sound, scenes and parameter tables fill
  the rest.
* 4,082 symbol names from `.rdata`/`.data` - the whole `ktol` online layer, the
  session classes, the EOS/Steam surface (`tools/symtab.py find CSession`).
  `Online::IsPlayerCoop1` / `IsPlayerCoop2` and `Arena::IsMatchType2vs2` (2v2 =
  four members) sit on the same session stack, while the co-op partner API is
  singular (`Online::GetCoopPartnerPlayerPosition`).

## Install / use

### Playing (player package)

```sh
tools/package.sh      # builds build/dinput8.dll and dist/nioh3p-player.zip
./install.sh          # this machine: copies dinput8.dll + nioh3p.ini into the game dir
```

`dist/nioh3p-player.zip` holds `dinput8.dll`, `nioh3p.ini` and a player-facing
`README.md` (source: `package/README.md`): where to put the files on Windows or
Proton, how to start a three-player session, and which log to send back. Every
player in the group installs it. `./install.sh` backs up a differing installed
`nioh3p.ini` (`nioh3p.ini.bak`, then timestamped) before replacing it;
`./install.sh uninstall` restores a stock game.

The shipped `nioh3p.ini` is the configuration three-player co-op was verified
with, and the DLL's built-in defaults are the same values, so a missing line
changes nothing:
`max_players=3 hook_eos=1 drop_lock_filter=1 drop_slots_filter=1 force_unlock=0`,
no dumps, `trace=0`. `max_players` accepts 0 (clamps off) or 2 to 4; any other
value is replaced by 3 and logged.

`nioh.exe` is identified by its PE headers. On any other `nioh.exe` build
(after a game update) the worker logs
`unsupported nioh.exe build … mod inactive - the game runs unmodified` and
installs no EOS, Steam or game hooks; the image dump still works.

**Player mode (`trace=0`)** installs only what changes behaviour: the Steam
matchmaking `CreateLobby`, `SetLobbyMemberLimit`, `SetLobbyData` (also the
metadata-guard trigger), the string, numerical and slots-available search
filters (the numerical one only forwards, but its key names reach the log),
`JoinLobby`/`LeaveLobby`;
the EOS `CreateLobby`, `SetMaxMembers`, `SetParameter`, `AddAttribute`; and the
game-side patches. Nothing on the per-packet P2P path is hooked, and the log
holds about a hundred lines per launch. The research build logged every P2P call
(~2,000 lines/s, 1.7 GB in a four-hour host session).

Log: `nioh3p.log` next to the DLL (or in `out_dir`); the previous launch is
kept as `nioh3p.prev.log`. If that directory is not writable the proxy falls
back to `%TEMP%\nioh3p.log` (never the Windows directory). A healthy player log
contains `preflight: exe=ok eos=… steam=… game_patch=ok log=…`,
`mode: player (trace=0)`, `game capacity hook: create request is clamped before
native member allocation` and `game visitor allowance: recruitment opens 2
visitor slot(s) (shrine setter 2)`. The `eos=` ratio counts only exports this
build actually calls; a `eos: N hook(s) have no call site in this build:` line
names the exports the game never references (`EOS_LobbyModification_SetMaxMembers`
in 1.24.08) and is informational, not a failure.

### Research mode (`trace=1`)

`trace=1` restores the full instrumentation: every Steam matchmaking, friends,
P2P and socket call, callback dispatch, process exit, EOS join/member-count/
lobby-info, with call-site names (`nioh3p.sites.txt`, copy it manually) and
backtraces in module-relative form:

```
[...] EOS_Lobby_CreateLobby: api=7 max=2 perm=2 presence=1 invites=0 bucket=Nioh
[...] EOS_Lobby_CreateLobby: raising MaxLobbyMembers 2 -> 3
[...] EOS_Lobby_CreateLobby: 5 frame(s): nioh.exe+0x5d915d nioh.exe+0x5e27a1 ...
```

`api=7 max=2 perm=2 invites=0 bucket=Nioh` is what the shipping game builds at
all six `EOS_Lobby_CreateLobby` call sites; the frame `nioh.exe+0x5d915d` is one
of them (`docs/RE_EOS_CALLSITES.md` §4.1). The proxy writes through the option
structs only for the ApiVersions it was verified with (`CreateLobby` 7,
`SetMaxMembers` 2, `SetParameter` 1); any other value is forwarded unmodified
and logged as `eos: <call> ApiVersion <n> != <expected>; forwarded unmodified`.
`dump_image=1` (with `redump_after_s`,
`dump_every_s`) writes the decrypted runtime image; `max_players=0` turns the
clamps off. Expect gigabytes of log per hour of online play with `trace=1`.

## Working on it without disturbing the desktop

`tools/quiet.sh` is what the development runs go through:

```sh
tools/quiet.sh --check     # is now a good moment? (exit 0 = yes)
tools/quiet.sh proxy       # harness run: hooks + clamping, ~15 s, no game needed
tools/quiet.sh game        # copy of nioh.exe: DRM unpack + image dump
tools/quiet.sh game --wait # wait for an idle moment first, then run
tools/quiet.sh --stop      # kill anything a previous run left behind
```

It guarantees:

* **The Steam installation is only read.** Everything happens in a copy under
  `~/.cache/nioh3p/` (prefix, game copy, dumps); the runner snapshots the game
  directory before and after and reports that the file list, sizes and mtimes are
  identical. Your prefix (`compatdata/485510`), saves and Steam config are never
  touched, and no Steam URL is ever launched.
* **No window and no focus stealing.** The harness renders to a private
  `Xvfb :9`; the real game goes through `gamescope --backend headless`, which
  builds a real Vulkan swapchain and creates no window (verified: `hyprctl
  clients` count unchanged across a headless `vkcube`). Xvfb alone is not enough
  for the game - it has no DRI3, so Vulkan presentation fails there.
* **Capped resources.** The whole run sits in a `systemd-run --user --scope` with
  `CPUQuota=200%`, `CPUWeight=20`, `MemoryHigh=8G`, `MemoryMax=12G` (verified:
  `cpu.max=200000 100000`) and runs `nice -n 19 ionice -c3`.
* **An idle gate.** It refuses to start while a Proton game is running, while
  gamescope runs, while a fullscreen window is in the foreground, while another
  app holds an idle inhibitor, or while the load average is above 60% of the core
  count - and `--wait` polls until that clears.

Two constraints are not ours to remove, and both are documented with evidence
in `docs/FINDINGS.md` 6.4:

* `nioh.exe` is wrapped in Steam DRM. A directly started copy dies on the stub;
  copying the Steam client into the throwaway prefix (`Program Files
  (x86)/Steam`) gets it running headlessly and it *does* write 50,782,208-byte
  dumps, but `SteamAPI_Init` still fails against that copy.
* VMProtect decrypts `.text` **page by page as the game runs** - 436 of 4,505
  pages in such a dump - so a snapshot only holds the code the run has already
  executed. The *online session* functions therefore need a
  Steam-authenticated run; `dump_every_s` takes periodic snapshots to capture as
  much as a run touches.

For a run Steam must start, `tools/quiet-launch.sh` is the out-of-view route:
set appid 485510's launch option to `.../tools/quiet-launch.sh %command%`; normal
play is unchanged, `tools/quiet-launch.sh --headless-on` makes the next launch
render headless (no window) inside the capped scope, `--headless-off` restores
normal play, `--dry-run` prints what would run.

A VM was surveyed as an alternative and rejected: the only free GPU is the iGPU
while the 9070 XT is `boot_vga` with both monitors, a guest would need its own
74 GiB install and a second Steam account, and the harness runs need no game at
all (see `docs/FINDINGS.md` 6.4 for the measurements).

## The headless two-instance rig

For tests that need **another live player** (host + joiner, invite behaviour,
lobby visibility) rather than a single local instance, `tools/rig/` runs extra
Nioh instances as full Steam clients: their own Steam root, their own COW-cloned
library and prefix. Steam's UI runs on Xvfb; the game renders on the **iGPU**
under headless gamescope, with no window on the desktop and nothing written in
your own Steam root. See `docs/RIG.md` for the launch options and observed
failures. `DXVK_WSI_DRIVER=xcb` was an invalid workaround and is no longer set.

## The experiment that answers the open question

With the mod installed on the **host** machine only:

1. Start the game, go to the Torii Gates, choose *Yokai Realm with a Companion*.
2. Have two other players (stock clients) try to join.
3. Send `nioh3p.log` back.

It answers three things at once: which mode-specific call site creates the lobby,
what the game thinks the member count is, and whether a stock client refuses a
three-member lobby. The LAN rig verified the shrine-offering path (Ochoko Cup,
then *Random Encounters* searches); other co-op modes are untested.

## Verified so far

* The proxy loads inside the real, protected `nioh.exe` under Proton, before the
  entry stub, and its log is written.
* `DirectInput8Create` is forwarded to the real DirectInput (harness: `hr = 0`).
* The EOS hooks install through the game's import table; the clamp is observed by
  the caller (`MaxLobbyMembers 2 -> 3`).
* Steam interface hooks bind only the versions Nioh requests: `SteamMatchMaking009`,
  `SteamFriends017`, `SteamNetworking006`, and `SteamNetworkingSockets009`
  (`docs/RE_STEAM_CALLSITES.md` §§2.1–2.2). Each hook uses an explicit vtable
  index; matchmaking’s `CreateLobby` (13) and `SetLobbyMemberLimit` (31) indices
  are documented there. Unsupported versions are forwarded without patching.
  The harness checks that unsupported matchmaking lookup is forwarded and the
  supported version clamps the member limit through native vtable calls.
* The runtime image dump works on `nioh.exe` itself, headlessly and **without
  Steam launching it**: with the Steam client copied into the throwaway prefix
  (`docs/FINDINGS.md` 6.4), `tools/quiet.sh game` writes 50,782,208-byte images
  to `~/.cache/nioh3p/dumps/`. The early snapshot holds 436 of 4,505 decrypted
  `.text` pages, the snapshot 20 s later all 4,505 - the game is fully unpacked.
* From that image (`docs/FINDINGS.md` 6.5): the six `EOS_Lobby_CreateLobby` call
  sites take `MaxLobbyMembers` from an object's `+4` field and pass bucket
  `"Nioh"`. `CSessionP2pOpenCreateParam::SetParam` is vtable slot 2: its
  `self+0x20` member limit is separate from its metadata attribute table.
* The game's own caller at `nioh.exe+0x5d915d` matches the runtime frame the mod
  logged, so live logs and the static image line up.

Reproduce with `tools/quiet.sh proxy` / `tools/quiet.sh game` (equivalently
`tools/smoketest.sh`, which they wrap: files are copied into
`~/.cache/nioh3p/smoke`, a throwaway prefix - nothing is installed, nothing in
the game directory is written). `./build.sh --harness` builds the harness pieces.

EOS safety regression (no Steam login required; uses a local SDK lifetime fixture):
`DISPLAY=:9 tools/smoketest.sh ownership`. The returned lobby-info allocation must
remain readable until the **game caller** releases it. The old proxy failed this
check; the corrected proxy passes. The real-SDK `proxy` smoke now uses the actual
EOS attribute layouts and member-count signature rather than mirroring the old
proxy's incorrect declarations. These checks do not establish multiplayer gameplay.


## Limits

* Verified only in the LAN rig with the proxy on all three clients, via the
  shrine offering and Random Encounters. A three-player mission has not been
  finished, and the party HUD lists one partner name.
* On the rig's single integrated GPU, the visitor who joins second can sit on
  the loading screen for ~5 minutes; the other two clients take the GPU. Given
  GPU time, it loads in about 30 s. The host may time that slot out in the
  meantime, and a visitor whose admission flag was lost this way is dropped
  when the first visitor leaves (`docs/FINDINGS.md`).
* Logs land next to the DLL by default (about a hundred lines per launch in
  player mode); set `out_dir` to put them somewhere else.
