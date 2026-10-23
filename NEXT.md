# Next

State file for resuming work.
The evidence lives in `docs/FINDINGS.md` and `docs/RIG.md`.
Git does not track `build/`, `dist/` or `.local/`.

## Branch four-player (2026-10-23)

Done on branch four-player:

- Player build from this branch: SHA-256 prefix `6db5351d`, 38,400 bytes, built
  by `./build.sh`.
- The proxy now also raises the shrine setter cap. It patches the byte at RVA
  0xd5fb4c, the immediate of the cmp at 0xd5fb4a (`cmp $0x3,%ebx`, bytes
  `83 fb 03`), to 4 when the allowance is 3, so the online shrine site can now
  pass the value 3 past the setter's cap check. With allowance 2 the game bytes
  stay stock.
- The smoke harness passed with `max_players` 3 and 4, in player and research
  mode, plus the ownership harness at both counts. The harness now reads
  `max_players` from `nioh3p.ini` to build its expectation. `NIOH3P_MAX_PLAYERS`
  appends an overriding `max_players` key in `tools/smoketest.sh`.
- Three-player regression with the branch build: host b plus visitors a and c,
  driven by `coop.py run --host b --visitors a c`. All three roles passed. The
  host log had `game create request: raising max members 2 -> 3 before
  allocation`. Every client logged `game visitor allowance: recruitment opens
  2 visitor slot(s) (shrine setter 2)`. The host shot
  `shots/4p-reg-3p-b.png` shows three player characters.
- Four-player run: all four lan slots ran `max_players=4`. The three-player ini
  is kept as `lan-*/nioh3p.ini.3p` in each slot. Every client logged `game
  visitor allowance: recruitment opens 3 visitor slot(s) (shrine setter 3)`,
  so the setter cap raise installed on all four. Host b logged `game create
  request: raising max members 2 -> 4 before allocation` at 13:44:01.
- Visitors joined in order a, c, d, driven by `coop.py run --host b --visitors
  a c d`. The driver relief (SIGSTOP pauses of the waiting visitors) paused a
  and c while d loaded. a and c passed the driver checks. d searched, found
  the lobby and sent JoinLobby at 13:56:28 (three JoinLobby lines in its log).
  The driver then hit its default 600 s wall timeout while d was still
  loading. d finished loading on its own and entered the host mission right
  after the driver timed out.
- Evidence shots: `shots/4p-mission-b.png` and `shots/4p-move3-b.png` show the
  host view with the host and two visible visitors; the third visitor is off
  camera (the driver trace records all four). Visitor a view with the host and
  the two other visitors in `shots/4p-hostmove-a.png`. Movement was shown both
  ways. After a, c and d each walked, `4p-move3-b.png` is the before frame and
  `4p-move-b.png` the after frame; the visitor positions differ between them.
  After the host walked, the visitor shots `4p-hostmove-a.png`,
  `4p-hostmove-c.png` and `4p-hostmove-d.png` were taken. Driver trace:
  `shots/coop-20261023-134257.jsonl`, 104 steps.
- Slot d identity: SteamID 76561198023713188, name Nioh-LAN-D, Epic emulator
  id ending in 04 (epicid `00000000000000000000000000000004`, productuserid
  `79165699f5c687119e1e19e0e2fc59b9`), save account 63447460. The Nioh
  Savedata Decryption Tool v1.3 converted slot c's `63447459` saves for
  slot d. Run it as `-i <c file> -sid 63447460 -o SAVEDATA.BIN`. Use the
  destination save folder as the working directory. The tool writes the
  output, then waits on stdin. Give it Enter or stop it once the file exists.
  While the tool runs, Proton also starts its Xalia helper process next to the
  game.
- Rig script changes: `tools/rig/run.sh` accepts slot d for lan (compat-d
  prefix, slot b client files). `tools/rig/nioh_nav.py` lists a, b, c and d.
  `tools/rig/coop.py` accepts two or three visitors and `--relieve` now takes
  several slots. `tools/rig/setup.sh` writes `max_players=4` into slot a's ini
  on this branch.
- The release DLL `48415ff0` in `dist/` and in the real Steam folder is
  untouched.

Open on branch four-player:

- The party HUD still names only one partner at four.
- The coop driver default timeout of 600 s is too short for a fourth client's
  load. Pass `--timeout 900` for four-player runs.
- Mission finish, deaths, revives, rewards and a new offering after a visitor
  leaves are untested at four. d's join needed the relief pauses on a and c.
- Decide with the user whether the shipped default stays 3 with 4 as an
  opt-in.
- Merging to master, a new release or any upload stays a decision for the
  user.

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
- VirusTotal: 5/71 generic heuristics (Avast, AVG, Avira, WithSecure, Cynet);
  Microsoft reports it clean. Report link is in `docs/FP_REPORTS.md`.
- A Windows 11 VM for Defender tests is stopped and kept in
  `~/nioh3p-test/winvm` (13 GB). Usage is in `docs/RIG.md`.

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
- The Experiential Labs key in `~/Documents/tempKey.txt` is world-readable
  (mode 644). Moving it is your decision.
- Smart App Control did not block the unsigned harness exe in the VM when it
  was forced on through the registry. A friend's UI-enabled Smart App Control
  can behave differently; their first launch log will show it.
