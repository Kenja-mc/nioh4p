# Next

State file for resuming work.
The evidence lives in `docs/FINDINGS.md` and `docs/RIG.md`.
Git does not track `build/`, `dist/` or `.local/`.

## Current build

- `dist/nioh3p-player.zip` holds `dinput8.dll`, `nioh3p.ini` and the player
  `README.md`. The DLL SHA-256 is
  `48415ff045d11e34cb8e76ca9fc17afa2c8965b3d4f1c447ad96e01c646223a7` (player
  build, 38,400 bytes). This frozen release predates Torii Gate support.
  Do not replace it with `tools/package.sh` without approval.
- The real Steam folder
  (`~/.local/share/Steam/steamapps/common/Nioh`) has the same DLL and the player
  ini installed. A save backup from before that install is at
  `~/Backups/nioh-savedata-20261021-174429.tar.gz`.
- The separate live-test candidate is `build/live-test/nioh3p-live-test.zip`.
  It contains the player DLL, the default three-player ini and test instructions.
  The DLL SHA-256 is
  `632684d0a17e457abfdad2c18cf827e312f511ed74a0a08ee3a9a7238c188a81`.
  `build/live-test/SHA256SUMS` includes both the zip and DLL hashes.
  No upload, installation or release replacement is authorized.
- Rig slots `lan-{a,b,c,d}` hold this integrated player build with
  `max_players=4`, `trace=0` and stock search filters. The rig is stopped.
  The older DLL copies remain at `lan-{a,b,c,d}/dinput8.dll.saved`
  (SHA-256 starts `6db5351d`) and `lan-{a,b}/nioh3p.ini.saved`.
- The integrated candidate passed three-player, four-player and repeat
  four-player Torii Gate entry on the LAN rig. The repeat run used the same
  processes after a Himorogi exit. No mission was completed.
  Details and evidence paths are in `docs/FINDINGS.md`.
- The candidate passed the Windows harness and a local Defender scan with no
  detections at definitions `1.461.144.0`. No public scan was made.
- The frozen release's VirusTotal result was 5/71 generic heuristics.
  That result does not apply to this candidate. See `docs/FP_REPORTS.md`.
- A Windows 11 VM for Defender tests is stopped and kept in
  `~/nioh3p-test/winvm` (13 GB). Usage is in `docs/RIG.md`.

## Integrated development

`master` contains the `friend-invite` and `four-player` branches.
The branch names remain for reference. Nothing is pushed or published.

- Friend invite (direct join): pump byte patch, CCallback vtable hook, join task
  0x140d73380. The open-task singleton at [manager+0xa8] is now created through
  the game's own factory 0x140d66910 when absent. That was the root cause of
  "join task object missing" on a freshly booted client. Built, boots clean.
  The end-to-end synthetic-invite join is still unverified.
- The host logs `Steam host lobby id=N` once per lobby. Use that id for
  `nioh3p.invite.txt`. The `Steam CreateLobby -> call N` value is an API call
  handle, not a lobby id.
- Torii Gate with three and four players works on the rig with the player
  build: all players reach the mission, no Ochoko Cup. Two companion hooks do
  it: the host mirrors the preparation packets 76, 74, 75 and 17 to companion
  slots 2 and 3, and later companions leave the shared mission-start counter
  to slot 1. Every player needs the mod. Details: `docs/RE_TOKOYO_COOP.md` and
  `docs/FINDINGS.md`, "Torii Gate with three and four players".
- The rig is driven by `tools/rig/rig.py` (up, down, status, state, deploy,
  ini, collect) and `tools/rig/coop.py` (Luna is the default backend). No Steam
  client is needed for the LAN slots. See `docs/RIG.md`.
- Playtest 2026-10-23, three real Steam accounts: the mod worked. The host
  raised 2 -> 3 members, friend A (story filter <= 5) found and joined the
  lobby and stayed 8 minutes. Friend B never saw the host because Nioh's
  story-progress search filter (integer_77, equal-or-less) hides hosts whose
  progress is above the visitor's setting — game design, not a mod bug.
  Workaround until invites are verified: visitors set the filter at or above
  the host's story progress.

## Next actions

1. Three players reached and played a live Steam mission on 2026-10-25
   (`docs/FINDINGS.md`). Still unproven: mission completion, the result screen,
   deaths, revives and rewards.
2. Run the next three-player session on the research build with `trace=1`.
   One death, one companion leave, and one rejoin attempt answer the open
   questions: `session:` lines carry `allow`, `sel` and the slot table, and
   `game tokoyo: profile slot N` shows the profile layout.
3. Read those lines against `docs/RE_TOKOYO_COOP.md`, "Next targets", then fix
   the rejoin path (`0x140d7b234`) and the companion selector (`root+0x4e8`).
   Do not patch either before the probe lines exist.
4. Investigate direct invites separately. The LAN research trigger takes the id
   from `Steam host lobby id=` in `nioh3p.invite.txt`. It does not prove that a
   real Steam invitation works. Direct invites are not required for this test.
5. Decide whether to replace the frozen release only after the live tests.
   A public release, upload, fresh VirusTotal submission or push needs approval.
6. Keep the existing vendor false-positive drafts in `docs/FP_REPORTS.md`.
   Results for the frozen DLL do not establish results for this candidate.
7. If the game fails to start, remove the candidate's `dinput8.dll`.
8. For the local rig, run `tools/rig/rig.py up b a c`, then
   `EXPLABS_KEY_FILE=~/.omp/agent/provider-keys/experiential-labs python3
   tools/rig/coop.py run --host b --visitors a c`. Add `d` for four players.
9. Review and commit only the Nioh harvest section in
   `~/Projects/ai-stack/docs/records/harvest/summary.md`.
   That file also contains unrelated uncommitted Cataclysm-BN work.
   Preserve it and the unrelated reference-document changes in that repository.
10. Publish only through a scrubbed mirror clone, never from this repository.
    This repository has no remote on purpose: the tracked files carry the real
    home path, the rig account names and ids, and the LAN SteamIDs. The public
    tree replaces `/home/<user>`, `<account-name-private-1>`, `<account-name-private-2>`, `<account-name-private-3>`, the four
    SteamID64 values and the save account ids `<accountid-private-1>`, `<accountid-private-2>`,
    `63447459`, `63447460` with `<you>`, `player-one`, `player-two`,
    `PlayerTwo`, `<steamid-lan-a>` to `<steamid-lan-d>` and
    `<accountid-a>` to `<accountid-d>`. Copy the tree, apply that map, run the
    grep gate, then commit on top of the public `master` and tag there.

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
