# Next

State file for resuming work.
The evidence lives in `docs/FINDINGS.md` and `docs/RIG.md`.
Git does not track `build/`, `dist/` or `.local/`.

## Current build

- `dist/nioh3p-player.zip` holds `dinput8.dll`, `nioh3p.ini` and the player
  `README.md`. The DLL SHA-256 is
  `48415ff045d11e34cb8e76ca9fc17afa2c8965b3d4f1c447ad96e01c646223a7` (player
  build, 38,400 bytes). The research build is `build/research/dinput8.dll`.
  Rebuild the zip with `tools/package.sh`. The zip hash changes with every build.
- The real Steam folder
  (`~/.local/share/Steam/steamapps/common/Nioh`) has the same DLL and the player
  ini installed. A save backup from before that install is at
  `~/Backups/nioh-savedata-20261021-174429.tar.gz`.
- Rig slots `lan-{a,b,c}` run the same build. The rig is shut down.

## Next actions

1. Send the zip to the two Windows friends. Tell them to look for
   `preflight: exe=ok`. Send the DLL SHA-256 above in the same message. The
   README tells them to compare the hash with `certutil`.
2. Run the first real session with three Steam accounts online. Collect
   `nioh3p.log` from all three. Also collect `nioh3p.prev.log` if the problem
   was in an earlier launch. Make sure that the log has no
   `unsupported nioh.exe build` line (version mismatch) and that the capacity
   and allowance lines are present.
3. These items are still untested with three players: finishing a mission,
   deaths and revives, rewards, and a new offering after a visitor leaves. The
   party HUD shows one name.
4. The VirusTotal rescan of the resource build is blocked by a captcha. Upload
   the file from a browser or with an API key.
5. The Nexus upload kit in `docs/NEXUS.md` is ready and is prepare-only. Publish
   the mod after the pre-publish checklist passes, including the FP reports.
6. Windows Defender and Smart App Control on the unsigned DLL are not testable
   here. If a friend's game stops starting, delete `dinput8.dll` to correct it.
7. For rig navigation, `tools/rig/coop.py run --host b --visitors a c` drives a
   co-op join end to end. Set `OPENROUTER_ENV_FILE` or `OPENROUTER_API_KEY`
   first. `docs/NAV_OPTIONS.md` lists the next speedups in order. The first is
   per-role save templates, so CONTINUE lands at the shrine or Starting Point.
   The second is a read-only memory state oracle (loader phase, session root) to
   replace fixed waits.

## Open decisions

- Research data is kept at `~/.cache/nioh3p/out/text.dis` (270 MB disassembly)
  and `~/.cache/nioh3p/dumps/`.
- The first C launch on the rig (2026-10-21 18:49) crashed in the intro movie.
  The cause was a null COM pointer in `nioh.exe+0x52f321` (DirectShow). A
  relaunch was fine. The Wine log is at
  `$RIG/logs-c/steam-485510.crash-movie-20261021.log`.
