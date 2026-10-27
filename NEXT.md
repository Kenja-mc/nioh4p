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
  After an accidental overwrite, reconstructed from a rig DLL and historical
  ini and README versions; the original archive and member hashes were not
  recorded, so content identity is unverified. Reconstructed zip: `50bc324f…`.
- The real Steam folder
  (`~/.local/share/Steam/steamapps/common/Nioh`) currently has DLL SHA-256
  `7e19577beec646310644bd7de5974598fe9bbf634f00c91d8e9ab8a346d6175c`.
  Confirm its ini and intended installation before changing either.
  A save backup from before the install is at
  `~/Backups/nioh-savedata-20261021-174429.tar.gz`.
- The separate live-test candidate is `build/live-test/nioh3p-live-test.zip`.
  It contains the player DLL, the default three-player ini and test instructions.
  The current candidate is DLL `750785b5…` (46,592 bytes) in zip `0ef866bf…`,
  from source `8622d20`. It passed the six-run smoke matrix at three and four
  players and a four-player Torii Gate formation with three visitors on the rig
  on 2026-10-26, and it is published as pre-release `v1.1.0-rc2`. The Windows
  harness and Defender pass listed for `7e19577b` has not been repeated on this
  build yet. The earlier candidates were DLL `7e19577b…` in zip `e5a3b4ea…`,
  DLL `7e19577b…` in zip `fd9df16c…` and DLL `632684d0…` in zip `ff0a08f0…`.
  `build/live-test/SHA256SUMS` includes both the zip and DLL hashes.
  `build.sh` is not reproducible: the same source built `99076046` (the
  three-player HUD evidence) and `750785b5` (everything else); the deploy for
  the four-player run raced the repack, so the rig verified the shipped bytes.
  Publishing a release needs approval each time.
- The `7e19577b` build passed three- and four-player Torii Gate formation on the
  rig on 2026-10-25 (three players: host d, visitors a and c; four players:
  host b, visitors a, c and d; stock filters) and the harness matrix at three
  and four players. It differs from `632684d0` by the fan-out bound, the trace
  throttle and the hook thread exit.
- A visitor death was exercised at four players: the run failed for the whole
  party about sixty seconds later and all four reached the result screen.
  Reviving a fallen companion and completing a mission on the success path are
  still unexercised. Evidence: `~/nioh3p-test/runs/rel-4p-death/`.
- Slot b's game needs its Steam client running. Its library and ini were
  restored to the frozen `48415ff0` build afterwards.
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
The branch names remain for reference. This private repository has no remote.
Both `v1.1.0-rc1` and `v1.1.0-rc2` are public prereleases; rc2's asset is
`0ef866bf…`, but its tag points to public commit `d412a368…`. Verify that
source corresponds to the asset before relying on the release source.

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

## Party HUD: rows for every companion

- Shipped in rc2: the second companion row (the game builds it hidden; the
  proxy unhides it). Verified with three players.
- Built overnight, default OFF behind the new `hud_row3=1` ini key: a third
  row for four players. Mechanism (all in `src/proxy/nioh3p_proxy.c`, no game
  bytes patched): create element id `0x73` through the game's own factory/ctor/
  init/register path, private vtable copy with seven block-swapping thunks
  (A<->C), piggyback on the native rows' update slot for per-frame drive,
  companion from the party-slot array, anchors from the HUD root sprite
  (`registry[0]`, groups `0x5f`/`0x60` = `player_01_pos`/`player_02_pos`),
  write ours with the dirty bit `[entry+0xec] |= 0x10`.
- Trials 8-11 reached the mission on four rig clients; screenshots show three
  names and a gauge, but do not establish that the gauge belongs to the third
  companion or tracks changing HP/Ki. The gauge may be the local player's.
  Evidence: `$HOME/nioh3p-test/runs/hud-probe-1026/row3-trial11-{a,b,c,d}.png`.
- Verify distinct companion identities and live damage updates on every client,
  gauge/name alignment, rejoin and element lifetime. Review the global A/C
  block swap, render/worker concurrency and the exception handler for safety.
- Trial 7 crashed two clients during bound update; no recurrence in trials 8-11
  does not establish a fix. Windows harness and Defender remain untested for
  post-rc2 builds. Keep the experimental feature disabled by default.

## Next actions

1. The result screen and a death are now verified on the rig; mission
   completion on the success path and reviving a fallen companion are not.
   The 2026-10-26 session reached a mission and probed the HUD but could not
   down or revive anyone: the coop driver stops once everyone is in, and it has
   no combat or revive table. Doing that needs in-mission input, either a new
   driver table or a manual session.
2. The owner chose the cloud side for account `<accountid-private-2>` and local files were
   moved to trash. An authenticated download and conflict resolution have not
   been verified; see `.local/tickets/rig-steam-cloud-conflict.md`.
3. Invite testing needs the research build. Its startup death is resolved:
   the game exits during Steam initialisation when no Steam client is up on
   the rig, so boot with `tools/rig/rig.py up --client …` (`docs/FINDINGS.md`,
   "Resolved: the Steam client, not the proxy"). Invites are compiled out of
   the player build.
4. Investigate direct invites separately. The LAN research trigger takes the id
   from `Steam host lobby id=` in `nioh3p.invite.txt`. It does not prove that a
   real Steam invitation works. Direct invites are not required for this test.
5. Verify rc2's public tag/source correspondence and release claims against its
   asset before any release correction; repeat Windows harness and Defender on
   the shipped build.
6. Keep the existing vendor false-positive drafts in `docs/FP_REPORTS.md`.
   Results for the frozen DLL do not establish results for this candidate.
7. If the game fails to start, remove the candidate's `dinput8.dll`.
8. For the local rig, run `tools/rig/rig.py up --client b a c`, then
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
