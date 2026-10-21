# Next

State file for resuming work. Evidence lives in `docs/FINDINGS.md` and
`docs/RIG.md`; the repo has no git, so this file records what is uncommitted.

## Current build

- `dist/nioh3p-player.zip`: `dinput8.dll` + `nioh3p.ini` + player `README.md`.
  DLL SHA-256 `fd4a58249fdadb208e1f10124cb786f8108f207876d278089e3f9b5dffe2df1c`.
  Rebuild with `tools/package.sh`; the zip hash changes with every build.
- The same DLL and the player ini are installed in the real Steam folder
  (`~/.local/share/Steam/steamapps/common/Nioh`). A save backup taken before
  that install: `~/Backups/nioh-savedata-20261021-174429.tar.gz`.
- Rig slots `lan-{a,b,c}` run the same build. The rig is shut down.

## Next actions

1. Send the zip to the two Windows friends. Send the DLL SHA-256 above in the
   same message; the README tells them to compare it with `certutil`.
2. First real session, three Steam accounts online: collect `nioh3p.log` from
   all three (and `nioh3p.prev.log` if the problem was in an earlier launch).
   Check for `unsupported nioh.exe build` (version mismatch) and for the
   capacity/allowance lines.
3. Still untested with three players: finishing a mission, deaths/revives,
   rewards, re-offering after a visitor leaves. The party HUD shows one name.
4. Not testable here: Windows Defender / Smart App Control on the unsigned DLL.
   If a friend's game stops starting, the fix is deleting `dinput8.dll`.

## Open decisions

- Over-engineering review (2026-10-21) listed about 30 removable lines in
  `src/proxy/nioh3p_proxy.c` (dead `g_steam_hooks`, one-caller wrappers
  `steam_apply_vtable`/`scan_and_patch`, the `known` pre-pass in
  `steam_apply_table`, a duplicate comment in `install_steam_hooks`). Not
  applied; any change needs a rebuild and a rig run because it alters the
  shipped DLL.
- Research data kept: `~/.cache/nioh3p/out/text.dis` (270 MB disassembly),
  `~/.cache/nioh3p/dumps/`.
