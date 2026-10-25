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
- Rig slots `lan-{a,b,c}` hold the release build; `lan-a`/`lan-b` currently
  hold the `four-player` research build instead (see below). The rig is shut
  down.
- VirusTotal: 5/71 generic heuristics (Avast, AVG, Avira, WithSecure, Cynet);
  Microsoft reports it clean. Report link is in `docs/FP_REPORTS.md`.
- A Windows 11 VM for Defender tests is stopped and kept in
  `~/nioh3p-test/winvm` (13 GB). Usage is in `docs/RIG.md`.

## Branch `four-player` (not in master)

- Friend invite (direct join): pump byte patch, CCallback vtable hook, join task
  0x140d73380. The open-task singleton at [manager+0xa8] is now created through
  the game's own factory 0x140d66910 when absent — the root cause of
  "join task object missing" on a freshly booted client. Built, boots clean;
  the end-to-end synthetic-invite join is still unverified. The host now logs
  `Steam CreateLobby -> lobby=%llu` so a rig join can be matched to the lobby.
- Torii hosting support: the visitor-allowance patcher gained the torii-side
  sites 0xd7de1f (second start-recruiting store) and 0xcfa515 (second setter
  call, stock 2), installs per site (the torii code decrypts only after that
  menu is used, and one dormant site must not block the others), and raises
  the setter's `cmp $3` cap when three visitors are configured. All eight
  patch sites are byte-verified against the settled dump
  (`~/nioh3p-test/logs-a/nioh3p_image_settled.bin`). Untested live.
- lan-a/lan-b hold this research build (dinput8.dll SHA-256 starts
  `0ec12d87`) with trace inis. The release copies are saved as
  `lan-a/nioh3p.ini.saved`, `lan-b/nioh3p.ini.saved` and the saved dinput8.dll
  pairs next to them (release DLL sha starts `6db5351d`); restore before any
  release testing.
- Playtest 2026-10-23, three real Steam accounts: the mod worked. The host
  raised 2 -> 3 members, friend A (story filter <= 5) found and joined the
  lobby and stayed 8 minutes. Friend B never saw the host because Nioh's
  story-progress search filter (integer_77, equal-or-less) hides hosts whose
  progress is above the visitor's setting — game design, not a mod bug.
  Workaround until invites are verified: visitors set the filter at or above
  the host's story progress.

## Next actions

1. Rig test the `four-player` branch (one boot of two slots): (a) invite —
   put `nioh3p.invite.txt` with the lobby id in the visitor game dir and
   expect `open task object created`, then `JoinLobby` and host admission;
   (b) torii — host from the Torii Gate menu and expect the
   `start recruiting (alt)` / `torii offering setter` lines with 3 visitor
   slots, no shrine offering used. Afterwards restore the release files from
   the `.saved` backups.
2. Send the zip to the two Windows friends. Tell them to look for
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
4. Send the false-positive reports to Avast/AVG, Avira, WithSecure and Cynet
   with the texts in `docs/FP_REPORTS.md`. Each vendor has its own web form.
   Keep the Defender draft unless Defender flags this build later.
5. The Nexus upload kit in `docs/NEXUS.md` is ready and is prepare-only. Publish
   the mod after the pre-publish checklist passes, including the FP reports.
6. Signing: no free option fits today (`docs/SIGNING.md`). Before a Nexus
   release, decide on a license and a public repository; both are needed for
   SignPath Foundation. The paid fallback is Certum open source (€49, real name
   on the certificate).
7. If a friend's game stops starting, delete `dinput8.dll` to correct it.
8. For rig navigation, `tools/rig/coop.py run --host b --visitors a c` drives a
   co-op join end to end, but the LLM backends are offline (the OpenRouter key
   was revoked). Until a key exists again, drive the rig manually
   (`tools/rig/nioh_nav.py goto <slot> <state> --skip` taps through lore,
   dialogs and menus; `/tmp/bdrive.py` sends raw keys to slot b).
   `docs/NAV_OPTIONS.md` lists the next speedups in order. The first is
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
- The Experiential Labs key in `~/Documents/tempKey.txt` is world-readable
  (mode 644). Moving it is your decision.
- Smart App Control did not block the unsigned harness exe in the VM when it
  was forced on through the registry. A friend's UI-enabled Smart App Control
  can behave differently; their first launch log will show it.
