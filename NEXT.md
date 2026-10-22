# Next

State file for resuming work. Evidence lives in `docs/FINDINGS.md` and
`docs/RIG.md`. `build/`, `dist/` and `.local/` are not tracked by git.

## Current build

- `dist/nioh3p-player.zip`: `dinput8.dll` + `nioh3p.ini` + player `README.md`.
  DLL SHA-256 `f239e4c1c8c227190ff2ef1978002c4e4571ab826f83164a57f7c40a1b053cd2`.
  Rebuild with `tools/package.sh`; the zip hash changes with every build.
- The same DLL and the player ini are installed in the real Steam folder
  (`~/.local/share/Steam/steamapps/common/Nioh`). A save backup taken before
  that install: `~/Backups/nioh-savedata-20261021-174429.tar.gz`.
- Rig slots `lan-{a,b,c}` run the same build. The rig is shut down.

## Next actions

1. Send the zip to the two Windows friends (tell them to look for `preflight: exe=ok`). Send the DLL SHA-256 above in the
   same message; the README tells them to compare it with `certutil`.
2. First real session, three Steam accounts online: collect `nioh3p.log` from
   all three (and `nioh3p.prev.log` if the problem was in an earlier launch).
   Check for `unsupported nioh.exe build` (version mismatch) and for the
   capacity/allowance lines.
3. Still untested with three players: finishing a mission, deaths/revives,
   rewards, re-offering after a visitor leaves. The party HUD shows one name.
4. Not testable here: Windows Defender / Smart App Control on the unsigned DLL.
   If a friend's game stops starting, the fix is deleting `dinput8.dll`.
5. Rig navigation: `tools/rig/coop.py run --host b --visitors a c` drives a
   co-op join end to end (set `OPENROUTER_ENV_FILE` or `OPENROUTER_API_KEY`).
   Next speedups from `docs/NAV_OPTIONS.md`, in order: per-role save templates
   so CONTINUE lands at the shrine / Starting Point; a read-only memory state
   oracle (loader phase, session root) to replace fixed waits.

## Open decisions

- Research data kept: `~/.cache/nioh3p/out/text.dis` (270 MB disassembly),
  `~/.cache/nioh3p/dumps/`.
- The rig's first C launch of 2026-10-21 18:49 crashed in the intro movie
  (null COM pointer in `nioh.exe+0x52f321`, DirectShow); a relaunch was fine.
  Wine log kept at `$RIG/logs-c/steam-485510.crash-movie-20261021.log`.
