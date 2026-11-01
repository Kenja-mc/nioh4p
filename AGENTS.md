# Agent guidance

## Scope and sources

This repository contains a co-op mod for Nioh: Complete Edition, Steam app 485510. Keep player instructions in [README.md](README.md) and [package/README.md](package/README.md). Use the source and configuration as the authority for current behavior. Use the documents below for evidence and research context. Historical observations do not prove that a new build works.

For a change, update affected callers and player instructions. Report only tests and live behavior that you observed. Keep these documents current in place rather than adding dated status entries.

## Source map

- Core networking, capacity, HUD and lobby hooks: the modules in `src/proxy/` (see `src/proxy/nioh3p_internal.h` for the layout: util, log, config, pe, patch, eos, steam, rules, gamepatch, gamehooks, hud, prep, main). Keep optional gameplay policy out of the core.
- Add-on loading and ABI: `src/proxy/addons.c` and `src/common/nioh4p_addon.h`. The core loads known DLLs from game-root `DLLMods/`, including when the core itself is a renamed plugin there.
- Gameplay modules: `src/addons/`. Each module owns its config and native hooks. `runtime.c` handles config and DLL entry; `world_helpers.h` holds shared world reads. Init returns a descriptor; the core installs guarded hooks later and retains the DLL.
- Exports and resources: `src/proxy/dinput8.def`, `src/addons/addon.def` and `src/proxy/nioh3p.rc`.
- Configuration: core `nioh3p.ini`; add-on templates in `package/addons/`. Trace and dump options require the research build.
- Build, install and package: `build.sh`, `install.sh`, `play.sh` and `tools/package.sh`. Preserve frozen release archives; test new outputs before replacing a release.
- Analysis: `tools/pe.py`, `tools/analyze.py`, `tools/rtti.py`, `tools/lnk.py`, `tools/symtab.py`, and `tools/mergedump.py`.
- Harness and rig: `tools/testgame/`, `tools/smoketest.sh`, `tools/rig/rig.py`, and `tools/rig/coop.py`.

## Build and test entry points

`./build.sh` builds standalone and plugin forms of the core, five optional DLLs, and research counterparts. `./build.sh --harness` also builds the test harness and fake online DLLs. The build needs zig cc or mingw-w64, plus `llvm-rc` and `llvm-cvtres`. `LLVM_RC` and `LLVM_CVTRES` can select those tools. Build changed source before running `tools/package.sh`: packaging can reuse existing binaries.

`tools/quiet.sh proxy` runs the harness without the game. `tools/quiet.sh game` runs a copied executable in an isolated prefix. `tools/quiet.sh --check` checks the idle gate, `--wait` waits for it, and `--stop` stops remaining processes. Set `NIOH3P_MAX_PLAYERS=3` or `4` to select capacity and `NIOH3P_TRACE=0` or `1` to select player or research mode. `DISPLAY=:9 tools/smoketest.sh ownership` runs the EOS allocation lifetime regression fixture. Harness success does not establish multiplayer gameplay.

For enemy-HP arithmetic or classification changes, run `tools/smoketest.sh enemy-health` in both player and research modes. This test loads the actual add-on DLL. It replays the native HP clamp and covers friendly or unavailable side fields. Separate in-game checks must establish which characters the installed hook changes.

For Steam-started headless runs, use `tools/quiet-launch.sh` as described in the rig documentation. For live installs, `NIOH_DIR` selects the game directory. The install script can rebuild and alter that directory. `./install.sh uninstall` leaves logs in place.

After ABI or loader changes, exercise core-only loading, each add-on alone, the full set, disabled configs and rejected configs. Verify installation and removal only in disposable `NIOH_DIR` fixtures before any authorized live install. A generic `LoadLibrary` fixture does not prove compatibility with a third-party loader. Gameplay proof needs the chosen add-ons installed on the actual test party.

## Research pointers

- Evidence, observed limits, runtime image dumps, and patch details: [docs/FINDINGS.md](docs/FINDINGS.md).
- Torii Gate formation, mission synchronization and preparation-menu UI: [docs/RE_TOKOYO_COOP.md](docs/RE_TOKOYO_COOP.md).
- Rig setup, live runs, isolation, and collection: [docs/RIG.md](docs/RIG.md). Harness architecture and profiles: [docs/RIG_HARNESS.md](docs/RIG_HARNESS.md). Navigation alternatives: [docs/NAV_OPTIONS.md](docs/NAV_OPTIONS.md).
- Steam and EOS interfaces and call sites: [docs/RE_STEAM_CALLSITES.md](docs/RE_STEAM_CALLSITES.md) and [docs/RE_EOS_CALLSITES.md](docs/RE_EOS_CALLSITES.md). Session flow: [docs/RE_SESSION_FLOW.md](docs/RE_SESSION_FLOW.md). Online keys: [docs/RE_KTOL_KEYS.md](docs/RE_KTOL_KEYS.md).
- Safety boundaries: [docs/SAFETY.md](docs/SAFETY.md). Antivirus reports: [docs/FP_REPORTS.md](docs/FP_REPORTS.md). Signing: [docs/SIGNING.md](docs/SIGNING.md).
- Local work: `NEXT.md` is ignored and may be absent. Treat it as scratch notes, not a shared task list; check its claims against source and evidence.

HUD proof: confirm four occupied native member slots, not just `CSession+0xc0` (which includes empty slots). Require three distinct companion bars on every settled screen, then exercise damage, healing, departure, re-entry and mission exit. Hook logs alone do not prove visible output. Identify the exact DLL before applying evidence to a build; the released candidate and current source can differ.

Lobby UI proof: capture every initial preparation screen before moving any cursor. Count occupied names, keep native action labels and other-action help, and check join/leave refresh plus the mission transition. The native status card still shows only the first companion. Local LAN proof does not establish four-account Steam play.
