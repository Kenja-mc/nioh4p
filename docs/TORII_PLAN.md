# Plan: three and four players through the Torii Gate

Goal: a host opens Yokai Realm with a Companion at the Torii Gate, two or three
other players join, and all of them load and play the same mission. No Ochoko
Cup is used. Three players must keep working on the shrine path, as now.

The game calls this mode Tokoyo Coop (常世同行). The code uses that name.

## What is known (rig, 2026-10-25)

Evidence: `docs/FINDINGS.md`, "Torii Gate companion hosting".

- The host creates a Steam lobby with 4 members. All seven allowance sites
  install at boot.
- Extra clients find and join that Steam lobby with the stock search filters.
- The host pairs with one companion only. Its preparation menu has one
  `Companion` field. Only that companion receives the mission choice and the
  start signal.
- Every other client stays on `Waiting for mission to be chosen`. This happens
  when it joins during the preparation countdown and also after the mission
  starts.
- The shrine path already put three players in one mission. So the session
  layer, the member arrays and the P2P transport can carry three. The limit is
  in the Tokoyo Coop logic above the session layer.

Names found in the settled image dump give the starting points:

| Area | Names |
| --- | --- |
| Menus | `Menu::TokoyoCoopMatching`, `Menu::TokoyoCoopMenu`, `Menu::TokoyoCoopPartner`, `Menu::TokoyoCoopOnlineStatus`, `Menu::TokoyoSearchCondition` |
| Script API | `Online::IsTokoyoCoopMode`, `Online::IsPlayerTokoyoCoop`, `Online::TokoyoCoopClearLeavePreparation`, `Online::TokoyoCoopForceRescue`, `Online::GetExistPlayerNum`, `Online::IsWaitCoopPlayer`, `Online::AddFlagWaitCoopPlayer` |
| Session | `CSessionP2pPreparation`, `CSessionP2pPreparationHandle`, `CSessionP2pModifySessionInfo`, `CSessionP2pLock`, `CSessionP2pPacketHandler` (handles `LobbyChatMsg_t`) |
| Packets | `CPacketJoinRequest`, `CPacketJoinResponse`, `CPacketModifySessionInfo`, `CPacketLock`, `CPacketGrantOwner`, `CPacketChangeOwner`, `CPacketLeave`, `CPacketBye` |

## Rules for this work

- Do the research on the saved disassembly, image dumps and research-build
  traces. Never attach a debugger to the game.
- Every code patch checks the exact original bytes before it writes. It logs
  one line per site, and it installs only when `max_players` is 3 or more.
- Test only in the rig or the Windows VM. Luna (`--backend luna`) drives every
  menu. Keep the rig Steam accounts Invisible and the games muted.
- Do not change the release DLL `48415ff0…` or the real Steam folder.

## Phase 1: record what the host sends to its companion

The Torii code is decrypted only after the menu runs, and `text.dis` is older
than that. So start with fresh material.

1. Boot B and A with the research build, `dump_image=1` and `trace=1`. Let Luna
   run `coop.py run --host b --visitors a` and wait until both are in the
   mission. Save the image dumps of both clients.
2. Disassemble the new dumps. Make sure that the seven allowance sites still
   match their bytes.
3. Add research-only trace lines (inside `NIOH3P_RESEARCH`) for these events:
   lobby chat messages sent and received, `SetLobbyData` keys during the
   preparation phase, lobby member join and leave events, and the type of
   each P2P packet. Log the type and the size, not the payload.
4. Run the three-client case once more with A and C searching together.
5. Write a timeline from the match to the mission load: each message, its
   sender, and its receivers. Mark the messages that A received and C did
   not.

Done when: the timeline names the message or lobby-data change that carries
the mission choice and the start signal, and shows why C does not get it.

## Phase 2: map the single-partner state

1. Find the vtables of the `Menu::TokoyoCoop*` classes through their RTTI
   records. Follow the update functions of `TokoyoCoopMatching` and
   `TokoyoCoopPartner`.
2. Find where the host decides that matching is done and starts the
   preparation countdown. The likely test is the member count reaching 2, or
   the visitor allowance at `+0x39c` reaching 0.
3. Find where the host stores its one companion: a member index, a Steam ID or
   an object pointer. Find the code that sends the mission choice and the
   start signal to it.
4. Find the implementations of the script API calls in the table above.
   `GetExistPlayerNum` and `IsPlayerTokoyoCoop` show how mission code counts
   players. `TokoyoCoopForceRescue` and the failure rule ("both players died")
   show what assumes exactly two.
5. Compare each finding with the shrine path, which already handles three.

Done when: `docs/RE_TOKOYO_COOP.md` lists every address with what it does,
byte-checked in the new dump, and states which code assumes one partner.

## Phase 3: choose the patch

Choose after Phase 2. These are the candidates, from the smallest change to
the largest:

- A: The start signal is a broadcast that C ignores because C is not the
  recorded partner. Then patch the client side so that every member in the
  lobby accepts the host's mission choice and start.
- B: The host sends the choice and the start to one member. Then patch the send
  path so that it loops over all lobby members.
- C: Matching ends at 2 members. Then hold the preparation countdown open until
  the lobby reaches `max_players` or the countdown ends, so that late searchers
  can still join.
- D: Fallback. Keep Tokoyo Coop for the host and the first companion. When the
  mission starts, open shrine-style recruitment without an Ochoko Cup, so that
  more players join as visitors through Random Encounters. This reuses the
  path that already works for three. The open questions are whether visitors
  and a Tokoyo host can share one mission, and what happens to rewards.

Done when: one option is chosen and written down in `docs/RE_TOKOYO_COOP.md`,
with the sites to patch and the expected log lines.

## Phase 4: build and prove it on the rig

1. Implement the chosen option in `src/proxy/nioh3p_proxy.c`, research build
   first, then the player build.
2. Run `./build.sh`, then the harness:
   `QUIET_NOW=1 NIOH3P_TRACE=0 tools/quiet.sh proxy`, the same with
   `NIOH3P_TRACE=1`, and `tools/quiet.sh ownership`.
3. Run `coop.py run --host b --visitors a c --backend luna`. Success: all three
   clients reach `in_mission`, and each shows the other two players.
4. Test the edges with three players: finish a mission, die and get revived,
   one player leaves and another joins, and check the rewards.
5. Add slot d for four players. The `four-player` branch (`030d265`) has the
   slot d rig changes (`run.sh`, `setup.sh`, `coop.py`, `testgame.c`). Merge
   those parts, then run `--visitors a c d`. Use relief pauses if the
   integrated GPU cannot load four games.
6. Make sure that the shrine path still takes two visitors.

Done when: three players and then four players finish a mission through the
Torii Gate on the rig. Results go in `docs/FINDINGS.md` and `docs/RIG.md`.

## Phase 5: Windows and real players

1. Run the harness in the Windows VM with Defender at its defaults.
2. Build the player zip only after the owner approves a new release. A new DLL
   gets a new hash and needs a new VirusTotal check.
3. Play one real session with three Steam accounts. Collect `nioh3p.log` from
   every player.

## Risks

- VMProtect decrypts `.text` in parts and late. Patch sites in the Torii code
  appear only after the menu runs, so the per-site installer must keep waiting.
- Mission scripts can assume exactly two players. Rewards, the failure rule
  and rescue can go wrong for the third and fourth player.
- If the countdown is held open, a join can start a desync while the mission
  loads.
- Four games on one integrated GPU load slowly. The host can time out a slot
  while a client loads.
