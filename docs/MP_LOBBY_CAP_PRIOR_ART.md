# Raising lobby capacity in Steam/EOS co-op games: prior art, and what applies to Nioh 1

Web research, 2026-10-17. Every claim carries a URL. `[INFERENCE]` marks anything I
could not verify from a primary source. Nioh-side facts are quoted from this repo's
`docs/FINDINGS.md` (measured on the installed build), not re-derived here.

The question this answers: Nioh 1's host asks Steam for `CreateLobby(type=2, cMaxMembers=2)`,
the mod raises that to 3, and the *joining* client - in both directions (modded client ->
stock host, stock client -> modded host) - never reaches `JoinLobby` at all: it calls
`RequestLobbyData` three times and stops. What has worked elsewhere, and what does that
imply here?

---

## 1. Steamworks lobby mechanics, from primary sources

### 1.1 The calls the mod already hooks

| Call | Load-bearing sentence | Source |
|---|---|---|
| `CreateLobby(ELobbyType, cMaxMembers)` | "**cMaxMembers** ... The maximum number of players that can join this lobby. This can not be above 250." Triggers `LobbyEnter_t` and `LobbyDataUpdate_t`; "the lobby is joined & ready to use at this point." | https://partner.steamgames.com/doc/api/ISteamMatchmaking |
| `SetLobbyMemberLimit(lobby, cMaxMembers)` | "The maximum number of players allowed in this lobby. This can not be above 250." Owner only. | https://partner.steamgames.com/doc/api/ISteamMatchmaking |
| `GetLobbyMemberLimit(lobby)` | "The current limit on the # of users who can join the lobby. Returns **0** if no limit is defined / if no metadata is available." | https://partner.steamgames.com/doc/api/ISteamMatchmaking |
| Lobby ceiling | "A single lobby can have up to 250 users in it, although typically most games have at most 2-16 players." | https://partner.steamgames.com/doc/features/multiplayer/matchmaking |
| `SetLobbyData` / `GetLobbyData` | Owner sets arbitrary keys; key ≤ 255 chars (`k_nMaxLobbyKeyLength`), value ≤ `k_cubChatMetadataMax` = **8192** bytes. "Each user in the lobby will receive notification of the lobby data change via a `LobbyDataUpdate_t` callback, and **any new users joining will receive any existing data**." | https://partner.steamgames.com/doc/api/ISteamMatchmaking ; https://raw.githubusercontent.com/Aviarita/steam_sdk/master/isteamfriends.h |
| `RequestLobbyData(lobby)` | "Refreshes all of the metadata for a lobby that you're not in right now." Triggers `LobbyDataUpdate_t`; returns true if the request reached the server. | https://partner.steamgames.com/doc/api/ISteamMatchmaking |
| `LobbyDataUpdate_t` | `m_bSuccess` = "true if the lobby data was successfully changed"; also the completion signal for `RequestLobbyData`. Fires for non-members only after `RequestLobbyData` (search results are a snapshot; a friend's lobby has *no* data until requested). | https://partner.steamgames.com/doc/api/ISteamMatchmaking ; https://partner.steamgames.com/doc/features/multiplayer/matchmaking |
| `JoinLobby(lobby)` | "Joins an existing lobby... Returns SteamAPICall_t to be used with a `LobbyEnter_t` call result." | https://partner.steamgames.com/doc/api/ISteamMatchmaking |
| `LobbyEnter_t` | `m_EChatRoomEnterResponse` is a `EChatRoomEnterResponse`; `m_bLocked` = "If true, then only invited users may join." | https://partner.steamgames.com/doc/api/ISteamMatchmaking |

### 1.2 `EChatRoomEnterResponse` - i.e. who decides "the lobby is full"

Full enum (steam_api.h, as published in the Steamworks API reference):

| Value | Meaning (verbatim) |
|---|---|
| `k_EChatRoomEnterResponseSuccess` = 1 | "Success." |
| `k_EChatRoomEnterResponseDoesntExist` = 2 | "Chat doesn't exist (probably closed)." |
| `k_EChatRoomEnterResponseNotAllowed` = 3 | "General Denied - You don't have the permissions needed to join the chat." |
| `k_EChatRoomEnterResponseFull` = **4** | "**Chat room has reached its maximum size.**" |
| `k_EChatRoomEnterResponseError` = 5 | "Unexpected Error." |
| `k_EChatRoomEnterResponseBanned` = 6 | "You are banned from this chat room and may not join." |
| `k_EChatRoomEnterResponseLimited` = 7 | "Joining this chat is not allowed because you are a limited user (no value on account)." |
| `k_EChatRoomEnterResponseClanDisabled` = 8 | "Attempt to join a clan chat when the clan is locked or disabled." |
| `k_EChatRoomEnterResponseCommunityBan` = 9 | "Attempt to join a chat when the user has a community lock on their account." |
| `k_EChatRoomEnterResponseMemberBlockedYou` = 10 | "Join failed - a user that is in the chat has blocked you from joining." |
| `k_EChatRoomEnterResponseYouBlockedMember` = 11 | "Join failed - you have blocked a user that is already in the chat." |
| `k_EChatRoomEnterResponseRatelimitExceeded` = 15 | "Join failed - too many join attempts in a very short period of time." |

Source: https://partner.steamgames.com/doc/api/steam_api#EChatRoomEnterResponse

`Full`, `Banned`, `Limited`, `CommunityBan`, `RatelimitExceeded`, `MemberBlockedYou` are
Steam-backend verdicts - the local game cannot produce them, it only reads the value out of
`LobbyEnter_t` (the SDK header says the response "will be set to `k_EChatRoomEnterResponseSuccess`
if the lobby was successfully joined, otherwise it will be `k_EChatRoomEnterResponseError`",
which is the older, simplified wording of the same mechanism).
So **capacity is enforced by Steam, using the lobby's own member limit** - if the limit is 2 and
two are in, `JoinLobby` returns `Full`. `[INFERENCE]` for the "returns Full" wording (no Valve
text states it explicitly for `JoinLobby`), but the enum value's definition plus the search
behaviour below make it the only consistent reading:

* "By default, we will not return lobbies that are already full" - https://partner.steamgames.com/doc/features/multiplayer/matchmaking
* `RequestLobbyList`: "will only return lobbies that are not full, and only lobbies that are `k_ELobbyTypePublic` or `k_ELobbyTypeInvisible`, and are set to joinable with `SetLobbyJoinable`" - https://partner.steamgames.com/doc/api/ISteamMatchmaking
* `AddRequestLobbyListFilterSlotsAvailable(nSlotsAvailable)`: "Filters to only return lobbies with the specified number of open slots available." - same page
* SDK header comment: "this will never return lobbies that are full" - https://raw.githubusercontent.com/Aviarita/steam_sdk/master/isteammatchmaking.h

Consequence: "open slots" and "full" are computed from the lobby's member limit, which is set
by the **host** at `CreateLobby` (and may be changed with `SetLobbyMemberLimit`, owner only).
Raising the host's advertised limit is therefore sufficient to make N slots real - there is no
client-side slot arithmetic to defeat.

### 1.3 What a joining client itself checks

* **Nothing, capacity-wise.** There is no Steam-side client check a game must satisfy before
  `JoinLobby`; the failure is returned *to* the game afterwards as `k_EChatRoomEnterResponseFull`
  (1.2). 
* **Invites are a Steam-side restriction, but they are the host's choice**: `k_ELobbyTypePrivate`
  is "The only way to join the lobby is from an invite"; `k_ELobbyTypeFriendsOnly` is "Joinable by
  friends and invitees, but does not show up in the lobby list". A 4th type,
  `k_ELobbyTypePrivateUnique` = 4, exists on the Web API side
  (https://partner.steamgames.com/doc/api/ISteamMatchmaking ,
  https://partner.steamgames.com/doc/webapi/ILobbyMatchmakingService).
* **`SetLobbyJoinable(false)` is absolute**: "If joining is disabled, then no players can join,
  even if they are a friend or have been invited." (same page)
* **But none of that is security**: a programmatic `JoinLobby` with the lobby ID succeeds even on
  `Private`/`FriendsOnly` lobbies. Facepunch.Steamworks issue #648 reproduces it on the
  underlying C++ SDK: "If a lobby is set to private or friends only, anyone can still join it if
  they have the lobbyId" - https://github.com/Facepunch/Facepunch.Steamworks/issues/648
* **Invite delivery is a callback the game may ignore**: `InviteUserToLobby` posts
  `GameLobbyJoinRequested_t` to the invitee (or launches with `+connect_lobby <id>`), and
  "**It's up to your game to decide to obey it or not.**" -
  https://partner.steamgames.com/doc/features/multiplayer/matchmaking
* **Lobby data cannot gate entry at the Steam level.** It is owner-written metadata; the game is
  free to read it and refuse. Steam's own stance is that lobby membership *is* the authentication
  ("Any user in a Steam lobby is already fully authenticated with the Steam back-end. There is no
  need for the game to do any more authentication steps with lobby users...") -
  https://partner.steamgames.com/doc/features/multiplayer/matchmaking
  This is exactly the "invite/password/ticket in lobby data" idiom (section 4).

### 1.4 The EOS side (Nioh's crossplay-on transport)

Nioh imports 124 `EOS_*` entry points and creates its lobby with a bucket id of `"Nioh"` and a
`PermissionLevel` of 0 or 2 (FINDINGS §3, §6.5). Mapping:

| EOS fact | Meaning | Source |
|---|---|---|
| `EOS_Lobby_CreateLobby(..., MaxLobbyMembers, PermissionLevel, ...)` | capacity is fixed at creation; `EOS_LobbyModification_SetMaxMembers` may change it later ("acceptable range ... between the number of users currently in the lobby and `EOS_LOBBY_MAX_LOBBY_MEMBERS`") | https://dev.epicgames.com/docs/epic-online-services/multiplayer/lobbies-and-sessions/lobby-interface/lobby-interface-reference |
| Service limit | "Max players in a lobby: **64**" | same page |
| `EOS_LPL_PUBLICADVERTISED` (0) | "This is a public lobby; **any user can find or use it at any time as long as it isn't full**." | https://dev.epicgames.com/docs/api-ref/enums/eos-e-lobby-permission-level |
| `EOS_LPL_INVITEONLY` (2) | "This type of lobby is private... Users can only join the lobby through an invitation from a user already inside." | same page |
| Full-lobby join failure | `EOS_Lobby_TooManyPlayers` = 9004, "Too many players to register with this lobby." (the enum has no separate `LobbyFull` code) | https://dev.epicgames.com/docs/api-ref/enums/eos-e-result |

Same shape as Steam: capacity is a *service-side* property of the lobby object, set by the host,
and the joiner is not asked. `[INFERENCE]` that `TooManyPlayers` is what a join of a full EOS
lobby returns - Epic documents the code but not the call path.

---

## 2. Prior art: mods that raise a game's player count

"Stock clients?" = could an unmodded client take part. Where the only source is the mod author's
own description, that is stated.

| Project | Game (link) | What changed | Stock clients? |
|---|---|---|---|
| `meepen/ror2-maxplayers-mod`, PilaDeGente | Risk of Rain 2 - https://github.com/meepen/ror2-maxplayers-mod ; https://old.thunderstore.io/package/ricantar/PilaDeGente/ | Host-side: Harmony-patch `SvMaxPlayersConVar.SetString` -> `RoR2Application.maxPlayers/hardMaxPlayers/maxLocalPlayers`, and hook `SteamworksLobbyManager.CreateLobby` to set `steam_lobby_max_members` at creation | **yes [INFERENCE]** - no author statement, but the code is host-only and the lobby's own limit carries the value (1.2/1.3) |
| BigLobby (polarathene) / BigLobby3 | Payday 2 - https://github.com/polarathene/biglobby ; https://github.com/payday-restoration/BigLobby3 | Edits `settings/network.network_settings` + BLT mod; **changes the search key so vanilla players cannot see/join modded lobbies**; re-routes Network-class messages | **no** - "The search_key ... prevents regular players without the mod joining"; "All players must use both the BLT mod and pdmod ... Make sure you are all using the same version!" |
| `[BTMP] More Players` | Vermintide 2 - https://steamcommunity.com/sharedfiles/filedetails/?id=2113204803 ; https://github.com/danreeves/vermintide-2-mods/tree/master/MorePlayers2 | Lua/VMF mod: **publishes `btmp_max_players` in lobby data** via `Managers.lobby:setup_network_options()`, `[BTMP]` name prefix, rebuilds party/UI/scoreboard for N slots | **no** - every player needs VMF + the mod; vanilla client hardcodes `MAX_NUMBER_OF_PLAYERS = 4` in `MatchmakingManager` |
| l4dtoolz (+ guides, ABM) | Left 4 Dead 2 - https://github.com/lakwsh/l4dtoolz ; https://github.com/accelerator74/l4dtoolz ; https://steamcommunity.com/app/550/discussions/0/348293230314251422/ | Server-side Metamod/SourceMod: `sv_maxplayers`, `sv_setmax`, and **`sv_force_unreserved 1` = stop serving Steam lobby reservations ("there will also be no lobby cookies")**; clients then connect directly | **yes** - "reserved games (starting from lobby) are hardcoded to allow only 4 players (coop). Only unreserved servers can do that." The Workshop "8 Player Lobby" item only changes the browser UI |
| Party Limit Begone | Baldur's Gate 3 - https://www.nexusmods.com/baldursgate3/mods/327 | Content mod for party size + a **`.bat` that patches the local `bg3.exe` multiplayer limit** | **unknown/unstated** - the patch is applied on one machine's executable; author warns events are hardcoded for 4 |
| `More Players In-Game` | Deep Rock Galactic - https://mod.io/g/drg/m/more-players-multiplayer-expander | Host applies the mod; DRG's own rules: "Players with Approved or Sandbox mods will not be able to join your unmodded game" (https://www.deeprockgalactic.com/modding-support-faq) | **yes for the cap itself [INFERENCE]** - description advertises "up to 999 players ... through Steam invites or the server browser", no client requirement stated; the host's advertised limit is what clients see |
| `LobbyExpansion` | GTFO - https://thunderstore.io/c/gtfo/p/Cactus/LobbyExpansion ; https://github.com/ADarkCactus/GTFO.LobbyExpansion | BepInEx/Harmony patches of `SNet_LobbyManager`, `SNet_SessionHub`, `SNet_PlayerSlotManager`, `PlayerManager`, `SteamMatchmaking`, +20 slot/UI patches | **no** - "All players must have LobbyExpansion installed." |
| vanilla server config | Terraria - https://terraria.wiki.gg/wiki/Server | `-players <n>` / `serverconfig.txt`, up to 255 | **yes** - server-side setting, not a lobby patch (host-and-play's Steam path is separate) |
| `MorePlayers` (ModioMori) | Gladio Mori - https://github.com/ModioMori/MorePlayers | Harmony patches `SteamManager.HostLobby`/`EpicManager.HostLobby`, sets `NetworkManager.singleton.maxConnections`, and **nothing client-side** | **yes - author claim + user corroboration**: "NOTE: Only the host of the match requires this mod! This will do nothing for clients." (README); "only the host needs the mod so you and randoms can join" (https://steamcommunity.com/app/2689120/discussions/0/4522262398818960202/) |
| `MorePlayers` (zelofi / SpindlesModding / MorePlayersFixed) | R.E.P.O. - https://github.com/zelofi/MorePlayers ; https://github.com/SpindlesModding/MorePlayers ; https://github.com/FeroxFoxxo/MorePlayersFixed | Host-side: patch `SteamManager.HostLobby` -> `SteamMatchmaking.CreateLobbyAsync(cap)` **and** set the Photon room's `RoomOptions.MaxPlayers` in `NetworkConnect.TryJoiningRoom` | **yes [INFERENCE from code]** - the room/transport cap is host-set (Photon), so clients need nothing; REPO is otherwise Photon-based, hence "PUN is not exactly super stable" at high counts |
| `MorePlayers` (semimodder) | R.E.P.O. - https://github.com/semimodder/MorePlayers (**now 404**) | Indexed description: "Raise the maximum player cap from 6 to any limit between 2 and 20. **Host-Only: Only the room owner (host) needs to install this mod.**" | **yes - author claim, unverifiable (repo deleted)** |
| `RoboUnion` | R.E.P.O. - https://thunderstore.io/c/repo/p/linkoid/RoboUnion | Host-side player-count adjuster, public + private lobbies | **yes - author claim**: "Only the Host needs to have RoboUnion installed." |
| `MorePlayers` | LOCKDOWN Protocol - https://www.nexusmods.com/lockdownprotocol/mods/69 (+ mod 79 "MaxPlayers") | Nexus description: "Increases Lockdown Protocol's player limit beyond the vanilla 16 cap. **Host-side only mod - clients don't need it.** Configurable up to 24 players (safe) or higher (experimental)" | **yes - author claim** (page returned HTTP 403 to my reader; text is the engine-indexed description) |
| `More Players - 16 Player Co-op` | Ready or Not - https://www.nexusmods.com/readyornot/mods/7299 | **Split design**: a `.pak` DataTable patch (**all players need**) + a **host-only UE4SS Lua script that raises the EOS/Steam lobby limit via EOSMulti** | **partially** - the lobby limit is host-only, the gameplay data patch is mandatory for everyone |
| `MorePlayers` (Penple, x753, Virality) | Content Warning - https://github.com/Penple/ContentWarningMorePlayers ; https://github.com/MaxWasUnavailable/Virality ; https://github.com/x753/Content-Warning-More-Players | Patches client-side constants and spawn logic (`MainMenuHandler.Start` 4->32, `SpawnHandler.GetSpawnPoint`, `BedBoss.AssignBed`, `PlayerHandler.AllPlayersAsleep`) | **no** - "All players must have the mod installed otherwise they will not spawn in the lobby and video extraction will fail"; Virality: "Required by all players to work properly!" |
| 8-player mod (SEPPDROID) | Phasmophobia - https://steamcommunity.com/sharedfiles/filedetails/?id=2252444415 | Memory patch of `Assembly-CSharp.dll` (pre-IL2CPP) | **no** - must be applied on every client |
| MoreCompany / LobbyImprovements / HoloCheck | Lethal Company - https://thunderstore.io/c/lethal-company/p/notnotnotswipez/MoreCompany/ ; https://lethal.wiki/extras/faq ; https://github.com/1A3Dev/LC-LobbyImprovements ; https://github.com/bricks04/HoloCheck | Lobby expansion (BepInEx/Harmony) + version/password gates and Steam-session-ticket validation | **no** - modding wiki: "Everybody trying to join the server has the mod installed. Everybody ... the same mod version"; LobbyImprovements' password feature is documented as "only joinable by people who have this mod" |
| `MorePlayers` (haayden) | How To Fish - https://github.com/haayden/howtofish-moreplayers ; https://www.nexusmods.com/howtofish/mods/3 | Host-side `SteamManager.CreateLobby(type, cap)` + `SetLobbyMemberLimit(lobby, cap)` + local transport cap | **contradictory**: Nexus/mirror says "Only the host needs this mod. Friends can join using an unmodded game client"; the author's GitHub README says "Everyone in the lobby needs this. The host's cap does not carry to clients." Unresolved |
| `More Players` | Jump Space - https://www.nexusmods.com/jumpspace/mods/19 ; `ITBMorePlayers` https://github.com/bandademan/ITBMorePlayers | Unreleased-mechanism host-side lobby mods | **yes - author claim only** ("Host only" / "Only the host needs the mod"); mechanism not readable (Nexus 403) |
| `MaxPlayerCount` | Valheim - https://thunderstore.io/c/valheim/p/Azumatt/MaxPlayerCount | Server-side BepInEx config for the dedicated server cap | n/a - server-side knob, not a host-only peer patch |
| Seamless Co-op / unseamless-coop | ELDEN RING - https://github.com/micthiesen/unseamless-coop | Mod-created session matched by a **shared password**; host creates the lobby, everyone running the mod joins | **Steam-level yes** (anyone with the lobby id can enter), **game-level no** |
| No-Lobby-Reservation | CS:GO - https://github.com/honey-malviya/No-Lobby-Reservation | Server-side SourceMod plugin so clients can join a `public`-lobby dedicated server **without a lobby id** | **yes** - removes the game's lobby-id requirement entirely |
| synthetic lobby tool | Squad - https://shuffleperson.com/blog/squad-reverse-engineering.md | External Node/`steamworks.js` process **creates a lobby for the game's appid**, writes the exact metadata keys the game reads (`RedpointEOSRoomId_s`, `NUMOPENPUBCONN`, ...), and hands out a `steam://joinlobby/393380/<id>` link | **yes - the stock game consumes the fabricated lobby and connects.** The tool never calls the victim's `JoinLobby`; it makes a lobby the victim's own client will join |

### 2.1 The pattern that decides what a stock client can do

1. **Capacity is service-side** (section 1.2/1.4). A mod that only changes what the *host* advertises
   (`cMaxMembers` / `MaxLobbyMembers` / Photon `MaxPlayers`) needs nothing on the client - the client
   has no cap of its own. Verified: Gladio Mori; strongly implied by R.E.P.O., RoR2, DRG, How To Fish
   (Nexus side), Ready or Not's Lua half.
2. **Every "all clients need it" case is a *game-logic* check on the client**, never a capacity check:
   a compiled-in constant or spawn/slot/`IsGameFull` test (Content Warning, GTFO, Lethal Company,
   Phasmophobia, Vermintide 2's `MAX_NUMBER_OF_PLAYERS`, Payday 2's search-key filter). Those mods
   are client mods because the *game*, not Steam, refuses the extra player.
3. **So the deciding question for a stock client is always: does the stock client's game logic accept
   what the host advertised?** Where it does, host-only works (case 1). Where it does not, the joiner
   must be modded (case 2) - or the gate must be satisfied from the host side (Payday 2 solved it the
   other way: it made the lobbies invisible to stock clients instead of modding every client).
4. **Steam/EOS never gate on lobby data**; a game that gates on data is doing so after the fact, and a
   tool that controls that data can control the client (Squad; and the host-side "publish the keys the
   joiner expects" idiom). Lobby passwords/version-numbers/tickets are all game-layer
   (LobbyImprovements, HoloCheck, In Silence password bypass
   https://steamcommunity.com/app/1361000/discussions/0/3010060319382008974/, Seamless Co-op).

---

## 3. What applies to this Nioh 1 setup

Facts in play (from `docs/FINDINGS.md` and the live runs recorded there):

* Steam transport, `SteamMatchMaking009`; host calls `CreateLobby(type=2 /*public*/, cMaxMembers=2)`;
  the mod clamps it to 3 (verified in the harness). The game never calls `SetLobbyMemberLimit`.
* Lobby data keys named in the executable: `ktol_invite_data`, `ktol_owner_id`, `ktol_owner_name`,
  and `ktol::sessionp2p::eos::session::SessionService::GetInvitationTicket`.
* Joiner-side live evidence: `RequestLobbyData(lobby)` x3 -> accepted, then **no `JoinLobby`**, no
  member reads. Stock friend -> modded host: no join activity on the host at all.
* Capacity can therefore *not* be the reason the join dies: `k_EChatRoomEnterResponseFull` can only
  be observed **inside** a `JoinLobby` call, and no `JoinLobby` happens (1.2, 1.3).
* The session layer underneath is three-slot-capable (FINDINGS 6.1/6.3: `[rbp+0x18] = 3`, per-slot
  allocation gated at index 3, `Online::IsPlayerCoop1/2`, 2vs2 arena support) - the "2" is a
  mode-policy value copied from an object field (`src->+4`) into `MaxLobbyMembers`.

| Technique | Evidence | Applies here? | Test cost |
|---|---|---|---|
| **T1. Host-only cap raise** (`cMaxMembers` 2->3) | Section 1.2 + Gladio Mori/R.E.P.O./RoR2 host-only precedent (section 2) | **Yes but not sufficient**: it makes the third slot real and makes the lobby non-full for a third player's search, yet the observed join never reaches `JoinLobby` | Already implemented and harness-verified (`tools/quiet.sh proxy`); proving it end-to-end needs a 2nd account |
| **T2. Instrument the joiner** (mod on *both* sides; log every `GetLobbyData`/`GetLobbyDataCount`/`SetLobbyData`/`GetNumLobbyMembers`/`GetLobbyMemberLimit` key+value and the `LobbyDataUpdate_t` payload; `JoinLobby`/`LeaveLobby` already hooked) | Steam docs: data for a non-member only arrives after `RequestLobbyData` + `LobbyDataUpdate_t` (1.1) - exactly the three calls the client makes; the mod already hooks slots 17/19/20/21/22/32 | **Yes, and it is the decisive next step** - it turns "the join died" into "it read key X, got Y, wanted Z" | Mechanism: harness-only (extend `tools/testgame/fake_steamclient.c` with a data store). Real answer: 2nd account + `tools/quiet.sh game` |
| **T3. Use the game's own invite path instead of Steam's "Join game"** | Docs: `InviteUserToLobby` -> `GameLobbyJoinRequested_t` on the invitee, and the game "decide[s] to obey it or not" (1.3); Nioh's `SessionService::GetInvitationTicket` + `ktol_invite_data` suggest an in-game invite/ticket step the game expects | **Yes, cheap and untested** - if the failing attempts used the overlay/`steam://joinlobby` path, the game may be waiting for its own invite handshake before `JoinLobby` | No code; 2 accounts, normal run (host invites from the in-game Torii Gate friend list) |
| **T4. Serve synthetic lobby data to the joiner** (spoof `GetLobbyData` values / add/overwrite keys the client looks for). Two flavours: (a) host-side publish extra keys via `SetLobbyData`; (b) client-side return a synthesised value for `ktol_invite_data`/`ktol_owner_id` | Lobby data is owner-written metadata the game gates on afterwards (1.3); Squad shows a controlled lobby's keys fully steer a stock client (section 2); In Silence / HoloCheck / LobbyImprovements show game-layer gates are bypassable only by controlling the data or the client | **Yes as a technique, but it is a joiner-side patch unless the gate is pure host data** - a stock client cannot be helped by (a) if the check is on data only the joiner can supply | Harness for the mechanism (fake vtable already proves decode->patch->forward); 2 accounts to prove it |
| **T5. Synthetic external lobby** (create a 485510 lobby from outside the game - `steamworks.js`/`steam_api64.dll` - with the `ktol_*` keys we choose, then observe what a client does) | Squad precedent (section 2); `k_ELobbyTypePrivateUnique`/Web API and Open Steamworks show the surface a wrapper can use (https://partner.steamgames.com/doc/webapi/ILobbyMatchmakingService , https://github.com/SteamRE/open-steamworks) | **Yes as a diagnostic** - it isolates "the client's decision" from "our host build", and can be run without the game on the host side. It cannot make the client join (no API forces another user's `JoinLobby`) | Medium: new small tool; 1 Steam account that owns 485510 for the lobby + 1 more for the client |
| **T6. Localhost / P2P-only test to separate transport from matchmaking** (Nioh's crossplay-off path also has `CRawSocketSteam`, and the EOS path has `EOS_P2P_*`; `EOS_Lobby_CreateLobby` can be clamped the same way) | FINDINGS 3/5: both transports hooked; EOS lobby cap 64, `PUBLICADVERTISED` = "anyone can find or use it as long as it isn't full" (1.4) | **Yes** - if the Steam path's gate is Steam-lobby-specific, the EOS path may behave differently; worth one comparative run | 1 account can at least log the host-side call sites; joining still needs 2 |
| **T7. Mod the joiner's decision point** (find the caller that reads lobby data and skips `JoinLobby` - it is reachable: `.text` dumps, `.pdata`, RTTI `CSessionP2pSessionInfo::OnLobbyDataUpdate`, and `RtlCaptureStackBackTrace` instrumentation already exist) | FINDINGS 5/6.5 (instrumentation + dumps + `CCallback<CSessionP2pSessionInfo, LobbyDataUpdate_t>`); HoloCheck/LobbyImprovements show that joiner-side gates are the normal blocker elsewhere | **Yes, highest fidelity** - and it is the only route that keeps the host stock-compatible while letting a *modded* third player in | High: needs the decrypted image + 2-3 accounts |
| **T8. Ship the third player *outside* the Steam lobby accounting** (keep the lobby at 2, connect the third over the ktol/EOS-P2P transport anyway) | L4D2 l4dtoolz `sv_force_unreserved` ("no lobby cookies"; clients connect directly) and CS:GO No-Lobby-Reservation show the idiom; **but Steam itself never lets a client skip its own game's `JoinLobby`** (1.3, Facepunch #648) | **No as stated**: in Nioh the client's own code refuses *before* `JoinLobby`, so "ignore the lobby" requires T7 first. EOS-only direct-connect would be a different, larger project (the game owns the socket layer) | Very high |
| **T9. Dedicated-server conversion** | L4D2/Source pattern (section 2); Nioh has no server binary and the session layer is peer-to-peer (`CSessionP2p*`) | **No** - no server exists to modify | n/a |

### 3.1 Which of these the existing harness can already exercise

* **Mechanism-level, no accounts, `tools/quiet.sh proxy`** (`tools/testgame/fake_steam_thunks.S`,
  `fake_steamclient.c`): clamping `cMaxMembers`/`MaxLobbyMembers` (done); returning a chosen value for
  any hooked **read** (`GetLobbyData`, `GetLobbyDataCount/ByIndex`, `GetNumLobbyMembers`,
  `GetLobbyMemberLimit`) - T2/T4(a/b) mechanics; refusing/succeeding `JoinLobby` with a chosen
  `EChatRoomEnterResponse`; emitting a synthetic `LobbyDataUpdate_t`. The stub library holds the
  implementation class + vtable, so this needs no Steam, no EOS and no game.
* **Game-level, 2 accounts, `tools/quiet.sh game` / `tools/quiet-launch.sh`**: T1, T3, T6, and the
  real reading of T2. The Steam DRM stub only unpacks when Steam launches the game, hence a real
  Steam run per side.
* **Needs a 2nd account that owns 485510** (or a friend) for *any* join: T1, T3, T4(proof), T5, T6,
  T7. Nothing in the repo can create a second authenticated Nioh client.

---

## 4. Ranked recommendations for this repository

1. **Log the joiner (T2).** Put the existing proxy on the joining machine and record every lobby-data
   key the client asks for, the value it gets, the `GetNumLobbyMembers`/`GetLobbyMemberLimit` reads,
   and the absence of `JoinLobby`. This is the only measurement that identifies the gate, and the
   hook points already exist (slots 17/19/20/21/22/32). Extend the harness stub with a data store
   first so the plumbing is proven off-line.
2. **Retry the join through the game's own invite path (T3)** before writing more code: host invites
   from the in-game friend/Torii Gate list so the joiner receives `GameLobbyJoinRequested_t`, which is
   the path `ktol`'s invitation ticket is built for. Zero code, one comparison run.
3. **If the gate is a lobby-data value the joiner can be made to accept (T4),** patch it at the
   boundary the mod already owns: (a) host `SetLobbyData` augmentation, or (b) client-side synthesis
   of `ktol_invite_data`/`ktol_owner_id`. Both are one-function changes in `nioh3p_proxy.c`, and the
   harness can prove the patch fires - only the live join proves the game agrees.
4. **Do the same for the EOS transport (T6)** once the Steam path is understood: `EOS_Lobby_CreateLobby`
   is already clamped, so the two paths can be compared with the same logging.
5. **Only if 1-4 show the refusal is inside the joiner's session code, extend to T7** (patch the
   caller of the skipped `JoinLobby`, using the dump + backtrace tooling). This is where the project
   ends up if a stock client must be let in - and it is a joiner-side mod either way.
6. **Keep T5 in reserve** as a diagnostic harness for lobby data (synthetic lobby with chosen keys),
   and **discard T8/T9** for now: there is no server to convert, and no Steam API can make another
   client skip its own logic.

---

## 5. What I could NOT verify

1. **Nioh prior art**: no 3+ player Nioh 1 or Nioh 2 co-op mod, and no `ktol` reverse-engineering
   writeup, exists on the indexed web. Searches run: `ktol`, `ktol session`, `ktol::sessionp2p`,
   `ktol EOS`, `"ktol_invite_data"`, `"ktol_owner_id"`, `Nioh 2 EOS`, `Wo Long EOS lobby` (zero
   relevant hits); Nexus sections for Nioh (34 mods) and Nioh 2 (no multiplayer/co-op-cap mod);
   GitHub and Reddit produced only trainers and mod-manager/plugin-loader projects
   (e.g. https://github.com/Gagan-apps/Nioh-2-The-Complete-Edition-Steamworks,
   https://www.reddit.com/r/Nioh/comments/rl7dbo/nioh_2_4_man_mod/). Nioh 2's Expeditions already
   supports 3 natively (https://nioh2.wiki.fextralife.com/Multiplayer), which is why nobody built one.
2. **Why Nioh's joiner stops after `RequestLobbyData`** - no public source; every statement about it
   here rests on this repo's own measurements.
3. **Steam returning `k_EChatRoomEnterResponseFull` from `JoinLobby`** - the enum's own definition and
   the full-lobby search filter imply it, but no Valve text states it for the join call. `[INFERENCE]`
4. **EOS's failure code for joining a full lobby** - `EOS_Lobby_TooManyPlayers` (9004) exists; Epic
   does not document which call returns it. `[INFERENCE]`
5. **Lockdown Protocol, Jump Space, Inside the Backrooms and Ready or Not half-claims** - Nexus pages
   returned HTTP 403 to the reader; the "host-side only" wording is the search-engine-indexed mod
   description, not text I read. Author claims only.
6. **`semimodder/MorePlayers` (REPO)** - repo returns 404; only its indexed description ("Host-Only")
   is available. The successor forks document the mechanism but not the host-only claim.
7. **How To Fish** - the two sources contradict each other outright (Nexus mirror: stock clients can
   join; author's GitHub README: everyone needs it). Unresolved.
8. **RoR2 and DRG stock-client joining** - no author statement; inferred from the host-only shape of
   the patches. `[INFERENCE]`
9. **Phasmophobia guide quote** - https://steamcommunity.com/sharedfiles/filedetails/?id=2252444415
   returned HTTP 429 to my reader; the quote ("the server will decide that you are not allowed to
   join") comes from a subagent's read of that page.
10. **Whether Steam's per-app backend hardening (Jake-Rich's comment in Facepunch #648: "they were
    able to update our app's backend ... automatically applied to newer apps") applies to a 2017-era
    app like 485510** - unknown, and it concerns invite/lock enforcement, not capacity.
11. **Valve's own lobby test tool** - the Steamworks SDK SpaceWar example
    (https://partner.steamgames.com/doc/sdk/api/example) is the official lobby client, but its lobbies
    live under appid 480, so it can validate Steamworks behaviour, not Nioh's appid. Tools that talk
    to *Nioh's* appid are all third-party wrappers (`steamworks.js`-style, Mimicry
    https://github.com/DAT1305/Mimicry, LobbyFilterRemover
    https://github.com/k-Knight/SteamApiLobbyFilterRemover); I found no ready-made lobby browser that
    can enumerate and join arbitrary 485510 lobbies with chosen `cMaxMembers`.
