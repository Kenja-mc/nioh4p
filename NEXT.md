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
- Rig slots `lan-{a,b,c}` hold the release build in principle. Now `lan-a` and
  `lan-b` hold the `friend-invite` research build and `lan-c` the
  `friend-invite` player build (see below). The rig is shut down.
- VirusTotal: 5/71 generic heuristics (Avast, AVG, Avira, WithSecure, Cynet);
  Microsoft reports it clean. Report link is in `docs/FP_REPORTS.md`.
- A Windows 11 VM for Defender tests is stopped and kept in
  `~/nioh3p-test/winvm` (13 GB). Usage is in `docs/RIG.md`.

## Branch `friend-invite` (not in master)

This branch holds the current work. The older `four-player` branch (`030d265`,
slot d and the first four-client mission) is not merged into it.

- Friend invite (direct join): pump byte patch, CCallback vtable hook, join task
  0x140d73380. The open-task singleton at [manager+0xa8] is now created through
  the game's own factory 0x140d66910 when absent. That was the root cause of
  "join task object missing" on a freshly booted client. Built, boots clean.
  The end-to-end synthetic-invite join is still unverified.
- The host logs `Steam host lobby id=N` once per lobby. Use that id for
  `nioh3p.invite.txt`. The `Steam CreateLobby -> call N` value is an API call
  handle, not a lobby id.
- Torii companion hosting (rig, 2026-10-25): all seven allowance sites install,
  the lobby is created with 4 members, and two players load a mission with no
  Ochoko Cup. A third client joins the Steam lobby but stays on `Waiting for
  mission to be chosen`, because the companion mode pairs the host with one
  companion. Details: `docs/FINDINGS.md`, "Torii Gate companion hosting".
- The decision-model driver now follows the Torii flow, drives all visitors at
  the same time, and can leave a mission (`docs/RIG.md`, "Decision-model
  driver"). Use `--backend luna`.
- lan-a/lan-b hold the research build; lan-c holds the player build. The
  release copies are saved as `lan-{a,b,c}/dinput8.dll.saved` and
  `lan-{a,b}/nioh3p.ini.saved` (DLL SHA-256 starts `6db5351d`). Restore them
  before any release testing.
- Playtest 2026-10-23, three real Steam accounts: the mod worked. The host
  raised 2 -> 3 members, friend A (story filter <= 5) found and joined the
  lobby and stayed 8 minutes. Friend B never saw the host because Nioh's
  story-progress search filter (integer_77, equal-or-less) hides hosts whose
  progress is above the visitor's setting — game design, not a mod bug.
  Workaround until invites are verified: visitors set the filter at or above
  the host's story progress.

## Next actions

1. Follow `docs/TORII_PLAN.md` (Torii Gate with three and four players). Start
   with Phase 1: record what the host sends to its one companion, and find why
   a third client never gets the mission choice and the start signal. Use the
   saved disassembly and research-build traces, not a debugger.
2. Rig test the friend invite: take the id from `Steam host lobby id=`, put it
   in `nioh3p.invite.txt` in the visitor game dir, and expect `open task object
   created`, then `JoinLobby` and host admission.
3. Send the zip to the two Windows friends. Tell them to look for
   `preflight: exe=ok`. Send the DLL SHA-256 above in the same message. The
   README tells them to compare the hash with `certutil`.
4. Run the first real session with three Steam accounts online. Collect
   `nioh3p.log` from all three. Also collect `nioh3p.prev.log` if the problem
   was in an earlier launch. Make sure that the log has no
   `unsupported nioh.exe build` line (version mismatch) and that the capacity
   and allowance lines are present.
5. These items are still untested with three players: finishing a mission,
   deaths and revives, rewards, and a new offering after a visitor leaves. The
   party HUD shows one name.
6. Send the false-positive reports to Avast/AVG, Avira, WithSecure and Cynet
   with the texts in `docs/FP_REPORTS.md`. Each vendor has its own web form.
   Keep the Defender draft unless Defender flags this build later.
7. The Nexus upload kit in `docs/NEXUS.md` is ready and is prepare-only. Publish
   the mod after the pre-publish checklist passes, including the FP reports.
8. Signing: no free option fits today (`docs/SIGNING.md`). Before a Nexus
   release, decide on a license and a public repository; both are needed for
   SignPath Foundation. The paid fallback is Certum open source (€49, real name
   on the certificate).
9. If a friend's game stops starting, delete `dinput8.dll` to correct it.
10. For rig navigation, use the Luna backend:
    `EXPLABS_KEY_FILE=~/.omp/agent/provider-keys/experiential-labs python3
    tools/rig/coop.py run --host b --visitors a c --backend luna`. The
    OpenRouter key for `mercury` is revoked. `docs/NAV_OPTIONS.md` lists the
    next speedups in order.

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
