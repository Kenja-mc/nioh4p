# nioh3p - three-player Nioh research mod

nioh3p is the reachable part of a three-player co-op mod for Nioh: Complete
Edition (Steam appid 485510, under Proton), with instrumentation and working
notes. It is for modders and reverse engineers who want a working three-player
co-op session and the data behind it.

Status: three-player co-op works in the isolated LAN rig. A host and two
visitors, all running this proxy, played one mission together. Each rendered the
other two, and each moved independently. An attack by one visitor was visible to
the host (`docs/FINDINGS.md`, "Three players in one mission"). Two cases stay
untested: authenticated online play with three separate Steam accounts, and
stock (unmodded) visitors.

## What is here

The repository holds the proxy source, the build and install scripts, the
analysis tools and the test rig. The table names each path and what it does. The
evidence for each claim is in `docs/FINDINGS.md`.

| Path | What it is |
|---|---|
| `src/proxy/nioh3p_proxy.c` | the mod: a `dinput8.dll` proxy for Windows x64, freestanding (KERNEL32 only) |
| `src/proxy/dinput8.def` | exports, so the real DirectInput keeps working |
| `src/proxy/nioh3p.rc` | `VERSIONINFO` + embedded application manifest (`.rsrc`) |
| `build.sh` | builds `build/dinput8.dll` (zig or mingw-w64, plus `llvm-rc`/`llvm-cvtres`) |
| `tools/pechecksum.py` | recomputes the PE optional-header checksum after the link |
| `install.sh` / `install.sh uninstall` | puts two files in the game directory, or removes them |
| `nioh3p.ini` | configuration: `max_players`, dump, hooks |
| `tools/pe.py`, `tools/analyze.py` | PE reader + `.pdata`-driven disassembly used for the analysis |
| `tools/rtti.py` | RTTI -> vtables -> methods (needs a runtime image dump) |
| `tools/lnk.py` | reads the `K300` archive containers: `list`, `info`, `scan`, `dump` |
| `tools/symtab.py` | symbol names out of the readable sections (`stats`, `find`, `dump`) |
| `tools/mergedump.py` | merges image snapshots for maximum decrypted coverage, with a per-section report |
| `tools/testgame/` | harness that imports `dinput8.dll` and EOSSDK exactly like the game does |
| `tools/smoketest.sh` | runs the harness or a copy of `nioh.exe` under Proton in a throwaway prefix |
| `tools/quiet.sh` | runs the smoke tests without disturbing the desktop (idle gate, offscreen display, capped cgroup) |
| `tools/quiet-launch.sh` | Steam launch-option wrapper: normal play as before, headless gamescope when a flag file is armed |
| `tools/package.sh` | builds `dist/nioh3p-player.zip` (DLL, player ini, player README) |
| `tools/rig/coop.py` | Nioh front-end for the rig harness: CLI, named hooks, `host`/`visit`/`run`/`decide`. See `docs/RIG.md` |
| `tools/rig/perceive.py`, `tools/rig/drive.py`, `tools/rig/profiles/` | reusable screenshot → decision model → key-table harness. Add a profile to drive another game/UI. See `docs/RIG_HARNESS.md` |
| `docs/RIG.md` | the headless three-instance test rig |
| `docs/SAFETY.md` | what the mod changes, what it never does, and how to check it yourself |
| `docs/RIG_HARNESS.md` | perception/driver/profile architecture, question specs, `decide` smoke check |
| `docs/NAV_OPTIONS.md` | ranked options for faster rig navigation (memory oracle, save templates, vision) |
| `docs/FINDINGS.md` | evidence, co-op limits, what a real 3-player mod needs |

## What the mod does

The proxy hooks the game and raises the lobby capacity. It also records data for
research. Each item below names one function.

1. Forwards DirectInput to the system `dinput8.dll`. The game loads it purely as
   a proxy. `nioh.exe` imports `dinput8.dll`, so the proxy loads before the game
   entry point runs. The launcher in the same folder also imports that DLL. The
   proxy stays inert there (no configuration, no log, no hooks).
2. Dumps the decrypted runtime image (research, `dump_image=1`, off in the
   player package). The `.text` section of `nioh.exe` is encrypted at rest
   (entropy 7.997). Only the entry stub decrypts it at runtime, so you cannot
   analyze the game code statically. The proxy waits for the decryption and
   writes `nioh3p_image_boot.bin` / `..._settled.bin` (50.8 MB, raw image at
   `0x140000000`, with a `.json` manifest). That image is the input for any
   further netcode work.
3. Raises the EOS lobby capacity. It patches the import-table slot of the game
   for `EOS_Lobby_CreateLobby` and raises `MaxLobbyMembers` to `max_players`. It
   also raises `EOS_LobbyModification_SetMaxMembers` if the game uses it. Each
   raise is logged with its original value, and the patch is re-applied if the
   game restores the slot. The Steam `CreateLobby`/`SetLobbyMemberLimit` path is
   raised the same way.
4. Instruments the whole lobby conversation (research, `trace=1` only). This
   instrumentation turns a play session into reverse-engineering data:
   * `EOS_Lobby_CreateLobby` / `EOS_Lobby_JoinLobby`: arguments plus a stack
     backtrace in module-relative form (`nioh.exe+0x...`). You can look up the
     game-side caller directly in the dumped image.
   * `EOS_LobbyDetails_GetMemberCount`: the member count the game actually sees.
   * `EOS_LobbyDetails_CopyInfo`: the `MaxMembers`/`AvailableSlots` of the lobby
     as a joining client sees them. This field decides whether a stock client
     will accept a three-member lobby.
   * `EOS_LobbyModification_AddAttribute`: the matchmaking attribute keys and
     values, that is, how the game labels its modes.
   * `EOS_LobbyModification_SetMaxMembers`: logged when present.
5. Opens the game's own session to a third member. This works on the supported
   executable only, and every patch checks the exact original bytes first.
   * Raises the member count of the host's `CSessionP2pOpenCreateParam` before
     the native member arrays are allocated.
   * Keeps the lobby compatibility metadata intact when the publisher assigns a
     descriptor to itself.
   * Changes the host's visitor allowance to `max_players - 1`. That allowance
     is the "one visitor per offering" constant stored when recruitment opens.
     The admission check of the host then accepts a second visitor instead of
     replying with a rejection.

## The data path and the symbols

You can read the data path and the symbols without running the game. Most of the
recent work went there. Details are in `docs/FINDINGS.md` 6.2/6.3.

* 20 `K300` containers, 13,382 entries, 71.19 GB, every entry validated:
  `tools/lnk.py list`. Scripts (archives 06/08/15/18) carry the mission logic and
  the 1,072-name engine API that the scripts can call. Textures (`GT1G`, `KSLT`,
  `KTSR`), models (`MDLRESPK`), effects, sound, scenes and parameter tables fill
  the rest.
* 4,082 symbol names from `.rdata`/`.data`: the whole `ktol` online layer, the
  session classes and the EOS/Steam surface (`tools/symtab.py find CSession`).
  `Online::IsPlayerCoop1` / `IsPlayerCoop2` and `Arena::IsMatchType2vs2` (2v2 =
  four members) sit on the same session stack. The co-op partner API is singular
  (`Online::GetCoopPartnerPlayerPosition`).

## Install / use

You need two files: `dinput8.dll` and `nioh3p.ini`. Both files go into the game
directory, the folder that holds `nioh.exe`. Every player in the group does the
same steps.

1. Run `tools/package.sh`. It builds `build/dinput8.dll` and
   `dist/nioh3p-player.zip`.
2. Unpack `dist/nioh3p-player.zip`.
3. Copy `dinput8.dll` and `nioh3p.ini` into the game directory that holds
   `nioh.exe`.
4. Start the game through Steam or Proton.
5. On this machine, run `./install.sh` instead of steps 2 and 3. It copies the
   same two files into the game directory.

### Building from source

`build.sh` builds `build/dinput8.dll`. The `--harness` flag also builds the fake
EOS and Steam DLLs and the test harness. Run both commands from the repository
root.

```sh
./build.sh              # build/dinput8.dll
./build.sh --harness    # plus the fake EOS/Steam DLLs and the test harness
```

`build.sh` uses zig cc (or mingw-w64). It needs `llvm-rc` plus `llvm-cvtres`
from LLVM for the resource section. If either tool is missing, the build stops
with an error instead of producing a resource-less DLL. Set `LLVM_RC=`/
`LLVM_CVTRES=` to point at other installs.

The DLL carries a `VERSIONINFO` block with the product name, the file
description and the `dinput8.dll` original filename. It also carries an
embedded application manifest (`RT_MANIFEST` id 1: `asInvoker`, Windows 10/11
`supportedOS`, per-monitor DPI). `tools/pechecksum.py` then writes the PE
optional-header checksum, which the linker leaves at 0.

Code patches use W^X. The patch code makes the pages `PAGE_READWRITE` for the
store. It flushes them with `FlushInstructionCache` and sets them to
`PAGE_EXECUTE_READ` afterwards instead of restoring them as RWX. The log opens
with `FILE_SHARE_READ` only, so another process cannot rewrite it while the game
runs. These hardening steps work against generic antivirus heuristics. They are
not a substitute for code signing. `docs/FINDINGS.md` has the VirusTotal context.

### Playing (player package)

The player package is the zip file that each player installs. Two scripts build
it and install it. The commands below do both jobs.

```sh
tools/package.sh      # builds build/dinput8.dll and dist/nioh3p-player.zip
./install.sh          # this machine: copies dinput8.dll + nioh3p.ini into the game dir
```

`dist/nioh3p-player.zip` holds `dinput8.dll`, `nioh3p.ini` and a player-facing
`README.md` (source: `package/README.md`). That README explains where to put the
files on Windows or Proton, how to start a three-player session, and which log
to send back. Every player in the group installs the package. `./install.sh`
backs up a differing installed `nioh3p.ini` as `nioh3p.ini.bak`, then with a
timestamp, before it replaces the file. `./install.sh uninstall` restores a
stock game.

The shipped `nioh3p.ini` carries the capacity configuration that the
three-player co-op test used. The built-in defaults of the DLL are the same
values, so a missing line changes nothing:
`max_players=3 hook_eos=1 drop_lock_filter=0 drop_slots_filter=0 force_unlock=0`,
no dumps, `trace=0`. The two `drop_*` filters ship at `0`, so the mod keeps the
lobby search rules of the game. Locked and full sessions stay hidden, as in the
unmodded game. Set either filter to `1` as an opt-in workaround when a host
cannot be found. `force_unlock=1` is the host-side alternative that advertises
the session as unlocked. `max_players` accepts 0 (clamps off) or 2 to 4. Any
other value is replaced by 3 and logged.

The proxy identifies `nioh.exe` by its PE headers. On any other `nioh.exe`
build, after a game update, the worker logs
`unsupported nioh.exe build … mod inactive - the game runs unmodified`. It
installs no EOS, Steam or game hooks. The image dump still works.

Player mode (`trace=0`) installs only what changes behavior. The Steam hooks are
`CreateLobby`, `SetLobbyMemberLimit`, `SetLobbyData` (also the metadata-guard
trigger), the string, numerical and slots-available search filters, and
`JoinLobby`/`LeaveLobby`. The numerical filter only forwards, but its key names
reach the log. The EOS hooks are `CreateLobby`, `SetMaxMembers`, `SetParameter`
and `AddAttribute`, plus the game-side patches. Nothing on the per-packet P2P
path is hooked, and the log holds about a hundred lines per launch. The research
build logged every P2P call (~2,000 lines/s, 1.7 GB in a four-hour host session).

The log is `nioh3p.log` next to the DLL (or in `out_dir`). The previous launch
is kept as `nioh3p.prev.log`. If that directory is not writable, the proxy falls
back to `%TEMP%\nioh3p.log`, never the Windows directory. A healthy player log
contains these lines:

* `preflight: exe=ok eos=… steam=… game_patch=ok log=…`
* `build: resource manifest present`
* `mode: player (trace=0)`
* `game capacity hook: create request is clamped before native member allocation`
* `game visitor allowance: recruitment opens 2 visitor slot(s) (shrine setter 2)`

The `eos=` ratio counts only exports that this build actually calls. A
`eos: N hook(s) have no call site in this build:` line names the exports that
the game never references (`EOS_LobbyModification_SetMaxMembers` in 1.24.08).
That line is informational, not a failure.

### Research mode (`trace=1`)

`trace=1` restores the full instrumentation. It logs every Steam matchmaking,
friends, P2P and socket call, callback dispatch, process exit, and the EOS join,
member-count and lobby-info calls. It also writes call-site names
(`nioh3p.sites.txt`, copy it manually) and backtraces in module-relative form:

```
[...] EOS_Lobby_CreateLobby: api=7 max=2 perm=2 presence=1 invites=0 bucket=Nioh
[...] EOS_Lobby_CreateLobby: raising MaxLobbyMembers 2 -> 3
[...] EOS_Lobby_CreateLobby: 5 frame(s): nioh.exe+0x5d915d nioh.exe+0x5e27a1 ...
```

`api=7 max=2 perm=2 invites=0 bucket=Nioh` is what the shipping game builds at
all six `EOS_Lobby_CreateLobby` call sites. The frame `nioh.exe+0x5d915d` is one
of them (`docs/RE_EOS_CALLSITES.md` §4.1). The proxy writes through the option
structs only for these verified ApiVersions (`CreateLobby` 7, `SetMaxMembers`
2, `SetParameter` 1). It forwards any other value unmodified and logs it as
`eos: <call> ApiVersion <n> != <expected>; forwarded unmodified`.
`dump_image=1` (with `redump_after_s`, `dump_every_s`) writes the decrypted
runtime image. `max_players=0` turns the clamps off. With `trace=1`, expect
gigabytes of log per hour of online play.

## Working on it without disturbing the desktop

`tools/quiet.sh` runs the development tests. It keeps the desktop usable. The
commands below select a mode.

```sh
tools/quiet.sh --check     # is now a good moment? (exit 0 = yes)
tools/quiet.sh proxy       # harness run: hooks + clamping, ~15 s, no game needed
tools/quiet.sh game        # copy of nioh.exe: DRM unpack + image dump
tools/quiet.sh game --wait # wait for an idle moment first, then run
tools/quiet.sh --stop      # kill anything a previous run left behind
```

It guarantees:

* The Steam installation is only read. Everything happens in a copy under
  `~/.cache/nioh3p/` (prefix, game copy, dumps). The runner snapshots the game
  directory before and after and reports that the file list, sizes and mtimes
  are identical. Your prefix (`compatdata/485510`), saves and Steam
  configuration are never touched, and no Steam URL is ever launched.
* No window and no focus stealing. The harness renders to a private `Xvfb :9`.
  The real game goes through `gamescope --backend headless`. That builds a real
  Vulkan swapchain and creates no window (verified: `hyprctl clients` count
  unchanged across a headless `vkcube`). Xvfb alone is not enough for the game.
  It has no DRI3, so Vulkan presentation fails there.
* Capped resources. The whole run sits in a `systemd-run --user --scope` with
  `CPUQuota=200%`, `CPUWeight=20`, `MemoryHigh=8G`, `MemoryMax=12G` (verified:
  `cpu.max=200000 100000`) and runs `nice -n 19 ionice -c3`.
* An idle gate. It refuses to start while a Proton game is running, while
  gamescope runs, or while a fullscreen window is in the foreground. It also
  refuses while another app holds an idle inhibitor or while the load average is
  above 60% of the core count. `--wait` polls until that clears.

Two constraints are outside the control of this project. Both are documented
with evidence in `docs/FINDINGS.md` 6.4:

* `nioh.exe` is wrapped in Steam DRM. A directly started copy dies on the stub.
  Copying the Steam client into the throwaway prefix (`Program Files
  (x86)/Steam`) gets it running headlessly. It does write 50,782,208-byte
  dumps, but `SteamAPI_Init` still fails against that copy.
* VMProtect decrypts `.text` page by page as the game runs (436 of 4,505 pages
  in such a dump). A snapshot therefore holds only the code that the run already
  executed. For that reason, the online session functions need a
  Steam-authenticated run. `dump_every_s` takes periodic snapshots to capture as
  much as a run touches.

For a run that Steam must start, `tools/quiet-launch.sh` is the out-of-view
route. Set the launch option of appid 485510 to
`.../tools/quiet-launch.sh %command%`. Normal play is unchanged.
`tools/quiet-launch.sh --headless-on` makes the next launch render headless (no
window) inside the capped scope. `--headless-off` restores normal play, and
`--dry-run` prints the command that it will run.

A VM was surveyed as an alternative and rejected. The only free GPU is the iGPU,
while the 9070 XT is `boot_vga` with both monitors. A guest needs its own
74 GiB install and a second Steam account. The harness runs need no game at all
(see `docs/FINDINGS.md` 6.4 for the measurements).

## The headless two-instance rig

Some tests need another live player rather than a single local instance. These
tests cover a host plus a joiner, invite behavior and lobby visibility. For
them, `tools/rig/` runs extra Nioh instances as full Steam clients. Each
instance has its own Steam root, its own COW-cloned library and prefix. Steam
UI runs on Xvfb. The game renders on the iGPU under headless gamescope, with no
window on the desktop and nothing written in your own Steam root. See
`docs/RIG.md` for the launch options and observed failures.
`DXVK_WSI_DRIVER=xcb` was an invalid workaround and is no longer set.

## The experiment that answers the open question

The open question needs a test on the host machine only. Install the mod on the
host machine alone. Do not install it on the other clients. Then do these steps:

1. Start the game, go to the Torii Gates, and choose *Yokai Realm with a
   Companion*.
2. Ask two other players with stock clients to try to join.
3. Send `nioh3p.log` back.

These steps answer three things at once. They show which mode-specific call site
creates the lobby. They also show what the game thinks the member count is, and
whether a stock client refuses a three-member lobby. The LAN rig verified the
shrine-offering path (Ochoko Cup, then *Random Encounters* searches). Other co-op
modes are untested.

## Verified so far

The build checks and the runtime observations below passed. Each item names one
check. The commands to reproduce them follow the list.

* The proxy loads inside the real, protected `nioh.exe` under Proton, before the
  entry stub, and it writes its log.
* The proxy forwards `DirectInput8Create` to the real DirectInput (harness:
  `hr = 0`).
* The EOS hooks install through the import table of the game. The caller
  observes the clamp (`MaxLobbyMembers 2 -> 3`).
* Steam interface hooks bind only the versions that Nioh requests:
  `SteamMatchMaking009`, `SteamFriends017`, `SteamNetworking006` and
  `SteamNetworkingSockets009` (`docs/RE_STEAM_CALLSITES.md` §§2.1–2.2). Each
  hook uses an explicit vtable index. The `CreateLobby` (13) and
  `SetLobbyMemberLimit` (31) indices of matchmaking are documented there.
  The proxy forwards unsupported versions without patching. The harness checks
  that an unsupported matchmaking lookup is forwarded and that the supported
  version clamps the member limit through native vtable calls.
* The runtime image dump works on `nioh.exe` itself. It runs headless and
  without Steam launching it. With the Steam client copied into the throwaway
  prefix (`docs/FINDINGS.md` 6.4), `tools/quiet.sh game` writes 50,782,208-byte
  images to `~/.cache/nioh3p/dumps/`. The early snapshot holds 436 of 4,505
  decrypted `.text` pages. The snapshot 20 s later holds all 4,505, so the game
  is fully unpacked.
* From that image (`docs/FINDINGS.md` 6.5): the six `EOS_Lobby_CreateLobby` call
  sites take `MaxLobbyMembers` from the `+4` field of an object and pass the
  bucket `"Nioh"`. `CSessionP2pOpenCreateParam::SetParam` is vtable slot 2. Its
  `self+0x20` member limit is separate from its metadata attribute table.
* The game-side caller at `nioh.exe+0x5d915d` matches the runtime frame that the
  mod logged, so live logs and the static image line up.

Reproduce with `tools/quiet.sh proxy` or `tools/quiet.sh game`. These wrap
`tools/smoketest.sh`, which copies files into `~/.cache/nioh3p/smoke`, a
throwaway prefix. The run installs nothing and writes nothing in the game
directory. `./build.sh --harness` builds the harness pieces.

EOS safety regression: no Steam login is required, because the check uses a
local SDK lifetime fixture. Run `DISPLAY=:9 tools/smoketest.sh ownership`. The
returned lobby-info allocation must remain readable until the game caller
releases it. The old proxy failed this check. The corrected proxy passes. The
real-SDK `proxy` smoke now uses the actual EOS attribute layouts and
member-count signature instead of mirroring the incorrect declarations of the
old proxy. These checks do not establish multiplayer gameplay.

## Limits

Three limits apply to the verified build. The items below name each limit.
`docs/FINDINGS.md` holds the detail for the first two.

* Verified only in the LAN rig, with the proxy on all three clients, via the
  shrine offering and Random Encounters. The group did not finish a
  three-player mission, and the party HUD lists one partner name.
* On the single integrated GPU of the rig, the visitor who joins second can sit
  on the loading screen for ~5 minutes. The other two clients take the GPU.
  Given GPU time, it loads in about 30 s. The host can time that slot out in the
  meantime. A visitor who loses the admission flag this way is dropped when the
  first visitor leaves (`docs/FINDINGS.md`).
* Logs land next to the DLL by default (about a hundred lines per launch in
  player mode). Set `out_dir` to put them somewhere else.
