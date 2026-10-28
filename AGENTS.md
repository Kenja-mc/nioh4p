# Agent guidance

## Scope and sources

This repository contains a co-op mod for Nioh: Complete Edition, Steam app 485510. Keep player instructions in [README.md](README.md) and [package/README.md](package/README.md). Use the source and configuration as the authority for current behavior. Use the documents below for evidence and research context. Historical observations do not prove that a new build works.

For a change, update affected callers and player instructions. Report only tests and live behavior that you observed. Keep these documents current in place rather than adding dated status entries.

## Source map

- Proxy and game hooks: `src/proxy/nioh3p_proxy.c`. It forwards DirectInput and applies executable-specific patches after byte checks.
- Exports and resources: `src/proxy/dinput8.def` and `src/proxy/nioh3p.rc`.
- Configuration: `nioh3p.ini`. The player build excludes research instrumentation. Trace and dump options require the research build.
- Build and install: `build.sh`, `install.sh`, `play.sh`, and `tools/package.sh`. The package script overwrites `dist/nioh3p-player.zip`; preserve a frozen release unless replacement is intended.
- Analysis: `tools/pe.py`, `tools/analyze.py`, `tools/rtti.py`, `tools/lnk.py`, `tools/symtab.py`, and `tools/mergedump.py`.
- Harness and rig: `tools/testgame/`, `tools/smoketest.sh`, `tools/rig/rig.py`, and `tools/rig/coop.py`.

## Build and test entry points

`./build.sh` builds the player DLL in `build/` and the research DLL in `build/research/`. `./build.sh --harness` also builds the test harness and fake online DLLs. The build needs zig cc or mingw-w64, plus `llvm-rc` and `llvm-cvtres`. `LLVM_RC` and `LLVM_CVTRES` can select those tools.

`tools/quiet.sh proxy` runs the harness without the game. `tools/quiet.sh game` runs a copied executable in an isolated prefix. `tools/quiet.sh --check` checks the idle gate, `--wait` waits for it, and `--stop` stops remaining processes. Set `NIOH3P_MAX_PLAYERS=3` or `4` to select capacity and `NIOH3P_TRACE=0` or `1` to select player or research mode. `DISPLAY=:9 tools/smoketest.sh ownership` runs the EOS allocation lifetime regression fixture. Harness success does not establish multiplayer gameplay.

For Steam-started headless runs, use `tools/quiet-launch.sh` as described in the rig documentation. For live installs, `NIOH_DIR` selects the game directory. The install script can rebuild and alter that directory. `./install.sh uninstall` leaves logs in place.

## Research pointers

- Evidence, observed limits, runtime image dumps, and patch details: [docs/FINDINGS.md](docs/FINDINGS.md).
- Torii Gate formation and mission synchronization: [docs/RE_TOKOYO_COOP.md](docs/RE_TOKOYO_COOP.md).
- Rig setup, live runs, isolation, and collection: [docs/RIG.md](docs/RIG.md). Harness architecture and profiles: [docs/RIG_HARNESS.md](docs/RIG_HARNESS.md). Navigation alternatives: [docs/NAV_OPTIONS.md](docs/NAV_OPTIONS.md).
- Steam and EOS interfaces and call sites: [docs/RE_STEAM_CALLSITES.md](docs/RE_STEAM_CALLSITES.md) and [docs/RE_EOS_CALLSITES.md](docs/RE_EOS_CALLSITES.md). Session flow: [docs/RE_SESSION_FLOW.md](docs/RE_SESSION_FLOW.md). Online keys: [docs/RE_KTOL_KEYS.md](docs/RE_KTOL_KEYS.md).
- Safety boundaries: [docs/SAFETY.md](docs/SAFETY.md). Antivirus reports: [docs/FP_REPORTS.md](docs/FP_REPORTS.md). Signing: [docs/SIGNING.md](docs/SIGNING.md).
- Current work and local artifact state: [NEXT.md](NEXT.md). Check its claims against source and evidence before reuse.

The four-player HUD remains unproven: a local test with `hud_row3=1` showed only two companion bars despite registration and update logs. Treat visible output, not hook logs alone, as the completion criterion for that feature. The released candidate and the current source can differ; identify the exact build before applying evidence to either.
