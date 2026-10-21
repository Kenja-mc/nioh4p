# Findings: Nioh: Complete Edition, and what "three-player" would take

Everything below is measured on this machine against the installed Steam build.
Raw facts first, then the implications.

## 1. The installation

| | |
|---|---|
| App | Nioh: Complete Edition, appid **485510**, depot 485511, buildid 9388498 |
| Path | `~/.local/share/Steam/steamapps/common/Nioh` (75 GB) |
| Executables | `nioh.exe` (26,943,456 B), `nioh_launcher.exe` (settings only) |
| Third-party DLLs in dir | `EOSSDK-Win64-Shipping.dll`, `steam_api64.dll`, `GfeSDK.dll`, `AnselSDK64.dll` |
| Game data | `archive/archive_00..19.lnk` (Koei LINK containers, 69.5 GB total) + `lfm_order_*.bin` manifests |
| Other | `movie/` (294 `.wmv`), `sound/`, `launcher_data/`, official manual PDFs |
| Config | `~/Documents/KoeiTecmo/NIOH/{nioh.ini,config.xml,Savedata}` |

`nioh.exe` is PE32+ x64 with 8 sections, `SizeOfImage` 0x306D718… (50,782,208 B at
runtime), entry point **RVA 0x303B310, i.e. inside `.bind`**, not `.text`.

## 2. The game's code is encrypted at rest

| Section | Entropy (file) | Note |
|---|---|---|
| `.text` (18,449,408 B) | **7.997** | code, encrypted |
| `.rdata` | 5.06 | readable: import table, strings, RTTI |
| `.data` | 4.21 | readable |
| `.pdata` | 6.86 | valid function table (72,232 entries) |
| `.bind` | 7.955 | loader/DRM stub, entry point |

Consequences, all verified:

* Linear disassembly of `.text` desynchronises; there are **zero** references to
  any imported function in it (18 MB of ciphertext).
* `.pdata` therefore points at ciphertext: 72,232 "functions" decode as garbage.
* Only the entry stub (`.bind`) is plaintext - it is the Steam DRM wrapper:
  `call $+5 / push rbx..r15 / mov rcx,[rsp+0x78] / sub rcx,5 / … / call`.
* Running `nioh.exe` outside a Steam context makes it exit immediately
  (exit code 53) **before** the section is decrypted.
* With `steam_appid.txt` next to a copied `nioh.exe` (Steam client running), the
  stub unpacks successfully, measured from inside the process:

```
decrypt-probe: .text delta=0.0%  runtime-entropy=7.997   (t+100 ms, still encrypted)
decrypt-probe: .text delta=99.6% runtime-entropy=5.923   (t+300 ms, plaintext)
```

**So: no static patching of game code is possible. Any mod must act at runtime
(or on data files). This is the single most important constraint.**

## 3. Online transport

`nioh.exe` imports 124 `EOS_*` entry points plus the Steam API:

| Area | EOS APIs used |
|---|---|
| Identity | `EOS_Auth_*`, `EOS_Connect_*`, `EOS_Platform_*` |
| Session | `EOS_Lobby_CreateLobby`, `JoinLobby`, `LeaveLobby`, `UpdateLobby`, `DestroyLobby`, `KickMember`, `PromoteMember`, `SendInvite`, `*_AddNotify*` |
| Discovery | `EOS_LobbySearch_*`, `EOS_LobbyDetails_*` |
| Data | **`EOS_P2P_SendPacket` / `ReceivePacket` / `AcceptConnection`** |
| Extras | `EOS_Stats`, `EOS_Achievements`, `EOS_Leaderboards`, `EOS_Presence`, `EOS_Ecom`, `EOS_Friends`, `EOS_UI` |

The EOS SDK DLL itself is **not** encrypted (normal code entropy 6.6, 574 exports
in `.text`), and the game's import table is readable plaintext: the slot for
`EOS_Lobby_CreateLobby` sits at **RVA 0x119B1E0** and holds the same address that
`GetProcAddress("EOS_Lobby_CreateLobby")` returns. That makes the EOS boundary
the one place where the game can be intercepted without touching its code.

Client-side classes visible through RTTI (which lives in `.rdata`/`.data`,
unencrypted; 2,324 descriptors once the parser handles template names - see
`analysis/classmap.txt`):

* Koei's own online library, namespace `ktol`: `ktol::online_service::eos::basic::BasicService`,
  `ktol::sessionp2p::eos::session::SessionService` - i.e. `CSessionP2p*`,
  `CProtocol`, `CPacket*` (`Hello`, `JoinRequest`, `JoinResponse`, `Reject`,
  `Leave`, `Bye`, `Ping`, `Pong`, `ChangeOwner`, `GrantOwner`, `Lock`,
  `ModifySessionInfo`, `Reliable`, `Unreliable`, `Archive`), `CSessionP2pMember`,
  `CSessionP2pSessionInfo`, `CSessionP2pSearch`, `CNetManager`
* **both transports**: `CSessionP2pProcessSteam`, `CSessionP2pSteam`,
  `CRawSocketSteam`, `CCallback/CCallResult<CSessionCreateHandle, LobbyCreated_t>`,
  `CCallResult<CSessionJoinHandle, LobbyEnter_t>`,
  `CCallResult<CSessionSearchHandle, LobbyMatchList_t>`,
  `CCallback<CSessionP2pSessionInfo, LobbyDataUpdate_t>`,
  `CCallback<CSessionP2pPreparationHandle, SteamRelayNetworkStatus_t>`
  (Steam matchmaking) **and** the EOS lobby/P2P path
* the Steam side is reached through `SteamInternal_FindOrCreateUserInterface`,
  not flat imports - the interface versions the game asks for are in the image:
  `SteamMatchMaking009`, `SteamNetworking006`, `SteamNetworkingSockets009`,
  `SteamNetworkingUtils003`, `SteamFriends017`, `SteamUser021`, `SteamUtils010`,
  `SteamController008`

So co-op runs over **Steam matchmaking/P2P when crossplay is off and over EOS
when it is on** (the online settings toggle documented on PCGamingWiki). A mod
that only raises the EOS lobby cap would therefore cover half the cases; both are
hooked now (section 5).

The lobby calls themselves are made from **lambdas held in `std::function`**
objects - the "6 call sites" are `std::_Func_impl_no_alloc<lambda_...,
SessionService*>` wrappers whose vtables sit together at `0x1401586150`-
`0x1401586310`, each containing one CreateLobby and one JoinLobby lambda. That is
why they share one code shape, and why the mode in use has to be read from a live
run rather than from the call sites alone.

## 4. What the co-op limit actually is

From the shipped manual (EN and JP, `launcher_data/pdf/manual_en.pdf`,
`manual_jp.pdf`), which is the authoritative statement:

* **Summon Visitor** (Ochoko Cup during a mission): "summon a Visitor - *another*
  player online who will join you as a co-op partner" - i.e. host + 1.
* **Yokai Realm with a Companion** (Torii Gates): the JP text says 「常世同行では
  プレイヤーが2人とも落命した状態になると…」 - "when **both** players have died" -
  i.e. exactly 2.

The shipped co-op modes expose two participants. This establishes the supported
design, not a proof that every underlying session or actor array has two slots.

## 5. What this mod does about it

Implemented and verified (`src/proxy/nioh3p_proxy.c`):

1. `dinput8.dll` proxy - `nioh.exe` imports `dinput8.dll` statically, so the DLL
   is mapped by the loader *before* the DRM stub runs. Every real DirectInput
   export is forwarded to the system DLL. Freestanding build (KERNEL32 imports
   only) because Proton's Wine ships no `api-ms-win-crt-*` apisets.
2. Runtime image dumper - waits until `.text` is decrypted, then writes the whole
   50,782,208-byte runtime image (`nioh3p_image_boot.bin` / `_settled.bin` plus a
   JSON manifest). **This is what makes the game's code analysable at all**; the
   netcode region of the decrypted image is `0x1405D3xxx`-`0x1405E4xxx`
   (EOS lobby/P2P call sites).
3. EOS lobby clamp - patches the game's import-table slot for
   `EOS_Lobby_CreateLobby` (and `EOS_LobbyModification_SetMaxMembers` when
   imported) and raises `MaxLobbyMembers` to `max_players` (default 3), logging
   the original value on every call and re-applying the patch if the game
   restores it. The game never calls `SetMaxMembers`, so `CreateLobby` is the
   only capacity lever.
4. **Steam matchmaking clamp.** The mod captures interface objects through the
   `SteamInternal_FindOrCreateUserInterface` import hook and patches explicit
   vtable indices for the exact versions requested by this game:
   `SteamMatchMaking009`, `SteamFriends017`, `SteamNetworking006`, and
   `SteamNetworkingSockets009`. Other version strings are forwarded without
   applying a hook table. The version evidence and game-side calls are mapped
   in `RE_STEAM_CALLSITES.md` §§2.1–2.2.

   `cMaxMembers` is clamped like the EOS field; interface lookups include their
   version string in the log, and matchmaking slots are re-applied if restored.
   Flat export instruction decoding has been removed: the emulator's exports
   are real functions rather than the shipped API DLL's small dispatch thunks,
   so their bytes cannot identify the requested interface layout reliably.

   Selected watched matchmaking methods:

   | method | offset | slot |
   |---|---|---|
   | `CreateLobby` | `0x68` | 13 |
   | `SetLobbyMemberLimit` | `0xf8` | 31 |
   | `SetLobbyData` | `0xa0` | 20 |
   | `GetLobbyData` | `0x98` | 19 |
   | `GetNumLobbyMembers` | `0x88` | 17 |
   | `GetLobbyMemberLimit` | `0x100` | 32 |
   | `SendLobbyChatMsg` | `0xd0` | 26 |

   `GetLobbyMemberByIndex` is also watched, at index 18; the MSVC virtual
   method's caller-owned SteamID return buffer is preserved. The compiled
   harness exercises unsupported-version forwarding before any supported
   lookup, native supported-version lobby calls, full-width identities, and
   asynchronous result delivery. The 2026-10-20 build and `tools/quiet.sh proxy`
   run completed with rc=0. These are local ABI/capacity checks, not a gameplay
   join.
5. **Online instrumentation** used to find the game-side limits:
   `EOS_Lobby_CreateLobby`, `EOS_Lobby_JoinLobby` (arguments + **stack
   backtrace**, module-relative so the caller can be read straight out of the
   image dump), `EOS_LobbyDetails_GetMemberCount` (logs the count the game
   actually sees), `EOS_LobbyModification_AddAttribute` (logs the matchmaking
   attribute keys/values, i.e. how modes are labelled) and
   `EOS_LobbyModification_SetMaxMembers`. Backtraces are taken with
   `RtlCaptureStackBackTrace`, resolved from ntdll at runtime.
6. **Game-side session changes** (exact supported executable only, each guarded
   by its original bytes or vtable entries): the host's create-parameter member
   count is raised before native allocation; exact descriptor self-assignment in
   the metadata publisher keeps its buffer; and recruitment opens
   `max_players - 1` visitor slots instead of one. Together these admitted a
   second visitor into a live mission; see "Three players in one mission".

Verified under Proton in a throwaway prefix (`tools/smoketest.sh`), with a
harness that imports `dinput8.dll` and EOSSDK exactly like the game:

```
hook: EOS_Lobby_CreateLobby hooked through the import table (14119b1e0 -> ...)
EOS_Lobby_CreateLobby: api=3 max=2 perm=1 presence=0 invites=1 bucket=nioh3p-test
EOS_Lobby_CreateLobby: raising MaxLobbyMembers 2 -> 3
TEST: MaxLobbyMembers after call = 3 (clamped by proxy, expected 3)
steam: SteamAPI_ISteamMatchmaking_CreateLobby at 0x... -> 1 slot(s) patched
TEST: Steam CreateLobby cMaxMembers seen by the fake service = 3
TEST: Steam SetLobbyMemberLimit seen by the fake service = 3
```

The Steam result comes from a stub pair in the harness that is shaped like the
real thing: `tools/testgame/fake_steam_thunks.S` holds the two thunks
byte-identical to the shipped DLL, `tools/testgame/fake_steamclient.c` is a
separate module holding the implementation class and its vtable (as
`steamclient64.dll` does), and `fake_steam.c` only plays the interface factory.
Recording happens in the "steamclient" module, so the clamped value proves the
proxy patched a vtable that lives in another module - both through the vtable and
through the flat thunk. Neither Steam nor EOS nor three accounts are involved.

## 6. Where the two-player cap sits in the code

From the decrypted image (`.pdata`-bounded disassembly of all 72,232 functions):

* `EOS_Lobby_CreateLobby` is referenced **6 times** in **6 distinct call sites**
  (`0x1405D3CC0`, `0x1405D74C0`, `0x1405D84E0`, `0x1405D9060`, `0x1405D9B00`,
  `0x1405DDFB0`), each with a matching `EOS_Lobby_JoinLobby` site ~0x260 bytes
  later - i.e. per-mode create/join paths. They share one compiler-generated
  shape (build the options struct on the stack, call through the import table,
  bump a counter), which is why they look like a generated wrapper layer; they
  are not called through a common dispatcher, so the mode in use has to be
  identified from a live run.
* The options struct built at `[rsp+0x20..0x40]` maps onto
  `EOS_Lobby_CreateLobbyOptions`: `LocalUserId` from an object field,
  `PermissionLevel` computed as 0 or 2, `bPresenceEnabled` from a flag,
  `BucketId` = the string **`"Nioh"`** (one bucket for every mode, so a bigger
  lobby stays discoverable), and
  **`MaxLobbyMembers` copied verbatim from a session-object field (`[rbx+4]`)**.

* The game **never imports `EOS_LobbyModification_SetMaxMembers`** (checked in the
  import table), so capacity is decided once, at `CreateLobby`.

All struct layouts used by the mod are checked against Epic's own headers
(`eos_lobby_types.h`), which corrected one guess: in `EOS_LobbyDetails_Info`
`AvailableSlots` sits at **+28** and `MaxMembers` at **+32** (a naive layout
guess would have read the wrong field). `EOS_LOBBY_MAX_LOBBY_MEMBERS` is **64**,
so EOS itself is never the ceiling.

At the EOS boundary, the requested "2" is copied from session/matching state
(`COnlineSession`, the `CSessionP2p*` family, `Matching@Menu@@`), not encoded
as a constant in the call itself. The API clamp raises the service request;
it does not establish native game capacity. The Steam host path allocates
its member arrays before reaching `CreateLobby`, so it also needs the early
create-parameter hook described in §6.1.

### 6.1 The session layer may already allow three

The RTTI in `.rdata` names the whole protocol stack, and those names have
vtables, so the methods are directly addressable:

* session/transport: `COnlineSession`, `CSessionCreate/Join/Leave/Invite`,
  `CSessionP2pOpen{,-CreateParam,-JoinParam,-Param}`, `CSessionP2pMember`,
  `CSessionP2pSessionInfo`, `CSessionP2pProtocol`, `CSessionP2pProcessSteam`,
  `CSessionSearch`, `CSessionP2pSearch`
* protocol: `CProtocol`, `CPacketHandler`, `CPacket` with
  `Hello`, `JoinRequest`, `JoinResponse`, `Reject`, `Leave`, `Bye`, `Ping`,
  `Pong`, `ChangeOwner`, `GrantOwner`, `Lock`, `ModifySessionInfo`,
  `Reliable`, `Unreliable`, `Archive`
* sockets/voice: `CRawSocket`, `CRawSocketSteam`, `CSteamVoiceContext`

`CSessionP2pOpenCreateParam::SetParam` (`0x1405F0FF0`) copies two distinct
counts from its input:

| destination | source | observed role |
|---|---|---|
| `self+0x20` | `options+4` | requested maximum members; used by the host allocator |
| `self+0x30` | `options+8` | metadata attribute count, with 32-byte descriptors at `self+0x38` |

The constants at `self+0x0c`, `+0x10`, `+0x14` and `+0x18` include 1024,
256, 1500 and 3. Their presence does **not** establish a three-player limit.
In particular, `self+0x18 = 3` is not the member limit. The `cmp $3` near
`0x1405F10B8` bounds an allocator implementation index, not a player slot.

The decisive native-capacity evidence is instead the create worker:
it allocates arrays from `self+0x20` before calling Steam. A live host initially
had native capacity 2 while Steam advertised 3. Clamping the request before
allocation produced native capacity 3 and a non-null extra-member array.

### 6.2 What the archive containers hold

The 60 files in `archive/` are 20 `K300` containers (the other 40 are
`archive_NN.lnk` companions of the C++ tooling, plus unrelated files) holding
13,382 entries and 71.19 GB. The format, read out of the files themselves and
validated on all 20 (every entry monotonic, 0x800-aligned, inside the file, and
the last one padding up to exactly the file size):

```
0x00  "K300" | u32 reserved
0x08  u64 entry count
0x10  u64 total size = file size
0x18  u64 0x800 (table block granularity)
0x20  entry[count], 32 bytes each:
        u64 offset   absolute, 0x800-aligned, monotonic
        u64 size     stored bytes
        u64 size2    == size in every entry of every archive
        u64 flags    0 in every entry (no compression marker)
first data byte = round_up(0x20 + count*32, 0x800) == entry[0].offset
```

`tools/lnk.py list|info|scan|dump` reads it. Content, classified by the first
bytes of the entries:

| archive | entries | size | payload |
|---|---|---|---|
| 00 | 240 | 18 MB | tables (`09 00 00 00 4c 00 00 00` header) |
| 01 | 4,350 | 18.3 GB | 3,641 of 4,350 entries start with zeros |
| 02 | 4 | 54 MB | `GT1G` textures (Koei G1T) |
| 03 | 516 | 5.8 MB | 500 entries zero-filled |
| 04 | 167 | 1.25 GB | `EFFRESPK` effects |
| 05 | 8 | 48 MB | `_N1G0000` header |
| 06 | 1 | 6 KB | one script |
| 07 | 47 | 243 KB | mixed, mostly zeros |
| 08 | 286 | 12.5 MB | scripts, 286/286 |
| 09 | 6 | 182 KB | `3SPK1000` / `_SPK` sound packages |
| 10 | 1,041 | 6.3 GB | tables |
| 11 | 262 | 3.3 GB | `MDLRESPK` models |
| 12 | 111 | 2.1 GB | `SCEN` scenes |
| 13 | 2,973 | 1.9 GB | `TLSK3100` (= `KSLT`) textures |
| 14 | 214 | 66 MB | zeros |
| 15 | 211 | 1.1 MB | scripts, 211/211 |
| 16 | 183 | 916 MB | `KTSR` textures |
| 17 | 2,084 | 35.9 GB | `MDLRESPK` models |
| 18 | 408 | 866 MB | scripts (187 of 408) |
| 19 | 270 | 35 MB | tables (`0f 00 00 00 40 00 00 00`, then a pointer array) |

The script entries (06/08/15/18) start with a build stamp - `0x18` then the
24-byte ASCII `2011-04-29T14:26:48+0900` - followed by records of
`u16 length, u32 hash, name`; the names are the *engine API the scripts may
call* (`umud::yield`, `Status::SetEnd`, `Object::GetType`, ...). 1,072 unique
`Class::Method` names appear across the script archives
(`analysis/script_api.txt`), 377 of them player/online-facing - including
`Online::IsPlayerHost`, `Character::GetSummonNum`, `Chara::IsCoopPlayerEnterBox`.
Mission scripts are online-aware by construction: all 211 scripts in archive 15
reference `iScript::StopReceiveOnlineSyncNumber`, and 208 reference
`Script::WaitPlayerApproache`.

There is no name table in a container, so entries are addressed by index. The
loader composes names itself - the exe still carries the format strings
`%s%s_%02d.bin` and `%s%s_%02d.lnk`, i.e. *prefix + shard number*, with a plain
`.bin` variant that suggests per-file overrides, but the prefix table lives in
the encrypted section (unverified).

### 6.3 The symbol table, and the session classes it labels

The game ships a symbol table in the readable `.rdata`/`.data`: 4,082 unique
names, including the whole `ktol` online layer and the EOS/Steam surface it
sits on (`analysis/symbols.txt`; 449 session/network-ish names in
`analysis/session_symbols.txt`, extracted with `tools/symtab.py`). It needs no image dump, and it answers the
"what shapes exist" question directly:

* host/guest/coop slots: `Online::IsPlayerHost`, `Online::IsPlayerGuest`,
  `Online::IsPlayerCoop`, `Online::IsPlayerCoop1`, `Online::IsPlayerCoop2`,
  `Online::IsPlayerTokoyoCoop`, `Online::GetExistPlayerNum`
* PvP on the *same* session stack: `Online::IsOnlinePlayVersus`,
  `Arena::IsMatchType1vs1`, `Arena::IsMatchType2vs2`, and the 1VS1/2VS2
  leaderboards and rankings - 2vs2 means the transport and the session layer
  already carry **four** members
* co-op partner handling is singular: `Online::GetCoopPartnerPlayerPosition`,
  `Online::IsCoopPartnerPlayerObjectExist`, `Online::IsPlayerCoop`, plus the
  mission-side flags `Mission::IsCooperating` / `Check::IsCooperating`
* summon slots: `Character::GetSummonSlotNum`, `Check::GetSummonSlotNum` (the
  visitor mechanism), `Character::GetSummonNum`
* the session classes themselves: `CSessionP2pOpenCreateParam::SetParam` /
  `::Cleanup`, `CSessionCreateHandle::OnLobbyCreate`,
  `CSessionP2pSessionInfo::OnLobbyDataUpdate`, `CSessionP2pModifySessionInfo::Update`,
  `CSessionSearchHandle::SearchSession`, `CSessionP2pCloseHandle::Terminate`

Together with the RTTI pass (`analysis/classmap.txt`, 2,324 classes with their
vtables and method addresses) the session layer can be labelled without
guessing:

| class | vtable | methods |
|---|---|---|
| `CSessionP2pOpenCreateParam` | `0x141586d80` | `0x1405f0e00` `0x1405f0ec0` `0x1405f0ff0` `0x1405f11c0` |
| `CSessionP2pSessionInfo` | `0x141586da8` | `0x1405f1c80` `0x1405f1e60` |
| `CSessionP2pSessionSearchInfo` | `0x141586d40` | `0x1405f2f50` `0x1405f39a0` |
| `CSessionP2pCloseHandle` | `0x141586f50` | `0x1405f50b0` `0x1405f5150` `0x1405f5160` `0x1405efb50` `0x1405efc00` |
| `CSessionP2pModifySessionInfo` | `0x1415876a8` | `0x1405fef10` `0x1405fef70` `0x1405fefe0` `0x1405f4da0` |
| `CSessionCreateHandle` | `0x1415879c0` | `0x1406046d0` `0x1406047e0` `0x1406047f0` `0x1405efb50` `0x140604550` |

The create-parameter object's third vtable method is `SetParam`
(`0x1405F0FF0`–`0x1405F11BA`). It writes requested maximum members at
`self+0x20`, separately from the constant 3 at `self+0x18`. The latter is
not evidence of a three-member request. The early request hook now raises
`self+0x20` before the Steam create worker allocates its native member arrays.

The same `.rdata` blob gives a way to label methods mechanically: it keeps runs
of name strings next to runs of 8-byte method pointers, and the 62-pointer run
at file offset `0x15856e0` contains exactly the four
`CSessionP2pOpenCreateParam` slots listed above.

**What still needs a runtime image dump.** Everything code-level: the `+4`
field of the session object that feeds `MaxLobbyMembers`, the
`CSessionP2pMember` array sizing, `CPacketModifySessionInfo`, and which of the
`CSessionP2pOpenCreateParam` slots is `SetParam`. `analysis/classmap.txt` and
`analysis/symbols.txt` survive from the earlier pass, but the *image dump itself
does not* - it had been written into the game directory and was deleted during
the uninstall, so one Steam-launched run (with `out_dir` set, so it lands
outside the install) is needed to regenerate it.

### 6.4 Out-of-view runs: the VM question, and what actually works

The development runs must not take over the desktop, so a subagent surveyed the
host for an invisible-run design. Result: **no VM** - a VM is the most expensive
option and does not remove the real blocker (the Steam DRM stub). Measured:

| Option | Verdict | Evidence |
|---|---|---|
| VM + GPU passthrough | rejected: the only free GPU is the iGPU, and the 9070 XT is `boot_vga` carrying both monitors, so passing it through takes the desktop away | `cat /sys/bus/pci/devices/0000:03:00.0/boot_vga` = `1`; `card0` (iGPU) has no connectors; IOMMU groups clean (no ACS override needed) |
| VM + iGPU passthrough | possible but pointless: +qemu/libvirt/OVMF/swtpm installs, ~40 GB guest OS, a second 74 GiB game install inside the guest, and it still needs a second Steam account that owns 485510 | `pacman -Qs qemu` shows only `qemu-user-static`; no `virsh`; no images |
| harness runs | need no VM and no game at all - fake EOS/steam DLLs + `testgame.exe` under a throwaway prefix | `tools/quiet.sh proxy`, ~13 s |
| **headless gamescope** | **the answer**: renders a real Vulkan swapchain with zero windows | `env -u DISPLAY -u WAYLAND_DISPLAY gamescope --backend headless -W 640 -H 480 -r 30 -- vkcube --c 40` -> `Starting headless backend`, rc=0; `hyprctl clients | grep -c Window` = 4 before, 4 after |
| Xvfb | fine for the no-render harness, **useless for the game**: no DRI3, so Vulkan presentation dies | `xvfb-run -a vkcube` -> `vulkan: No DRI3 support detected - required for presentation`, then SIGSEGV |

`tools/quiet.sh game` therefore runs the game inside `gamescope --backend
headless` (falling back to Xvfb only when gamescope is absent); caps are
unchanged and were re-verified inside that wrapper (`cpu.max=200000 100000`,
`cpu.weight=20`, `memory.high=8G`). One practical trap: a killed gamescope can
leave children holding stdout, so a wrapped run must never be piped - the
runner writes to a log file and uses `timeout --foreground -k 10` instead, which
is what keeps a headless run bounded.

For a run that Steam itself must start, `tools/quiet-launch.sh` is a launch-option
wrapper: set appid 485510 to `.../tools/quiet-launch.sh %command%` and it
reproduces the invocation this machine already uses
(`ENABLE_GAMESCOPE_WSI=0 gamescope -f -w 1920 -h 1080 -W 1920 -H 1080 -r 120 --`)
for normal play, and switches to headless gamescope under a capped scope when
`~/.cache/nioh3p/headless.flag` exists (`--headless-on` / `--headless-off` /
`--status` / `--dry-run`). Launch options are per-title, hence the flag file
rather than a second launch-option line.

**A dump without Steam at all** - the DRM stub only unpacks when Steam is in the
chain, and the reason is precise: the copy's own log says
`[S_API] SteamAPI_Init(): Sys_LoadModule failed to load:
C:\Program Files (x86)\Steam\steamclient64.dll`. Copying the Steam client into
the throwaway prefix (below) let the process live and *did* produce 50,782,208-byte
dumps headlessly, but `SteamAPI_Init` still fails against that copy, so the run
never authenticates and the dump's `.text` stays mostly encrypted:

```sh
SC="$HOME/.cache/nioh3p/smoke/pfx/pfx/drive_c/Program Files (x86)/Steam"
mkdir -p "$SC" && cp ~/.local/share/Steam/steamclient64.dll "$SC/"
tools/quiet.sh game
```

Measured on such a run: the **boot** snapshot holds 436 of 4,505 `.text` pages
changed relative to the encrypted on-disk image, while the **settled** snapshot
taken 20 s later holds **4,505 of 4,505** - entropy 5-6 on every page, i.e. real
code. So the unpacking is a bulk pass during the first seconds of the run, and a
snapshot taken too early simply misses it; `dump_every_s` (periodic snapshots)
exists so a run's coverage is never a guess. Both dumps are 50,782,208 bytes and
live in `~/.cache/nioh3p/dumps/`, i.e. **a complete decrypted image of the game
is available without Steam and without a window** - everything in 6.5 below was
read from it.

Command that reproduces it:

```sh
tools/quiet.sh game                 # headless gamescope, capped, idle-gated
python3 tools/mergedump.py --report ~/.cache/nioh3p/dumps/*.bin
objdump -D -b binary -m i386:x86-64 --adjust-vma=0x140000000 \
        --start-address=0x1405f0ff0 --stop-address=0x1405f11c0 \
        ~/.cache/nioh3p/dumps/nioh3p_image_settled.bin
```

### 6.5 What the decrypted image shows on the session path

Read from `nioh3p_image_settled.bin` (all addresses are VAs at base
`0x140000000`).

**The six `EOS_Lobby_CreateLobby` call sites build their options on the stack,
and `MaxLobbyMembers` comes from an object field.** Around the site the runtime
log already reported (`nioh.exe+0x5d915d`, i.e. `0x1405d915d`):

```
1405d90fa  mov 0x4(%rbx),%eax        ; src->+4
1405d90fd  mov %eax,0x30(%rsp)       ; options.MaxLobbyMembers (options base = rsp+0x20)
1405d9113  mov %rcx,0x28(%rsp)       ; options.LocalUserId
1405d9123  mov %ecx,0x34(%rsp)       ; options.PermissionLevel
1405d912a  mov %eax,0x38(%rsp)       ; options.bPresenceEnabled
1405d9101  lea 0x...,%rax / 1405d9108 mov %rax,0x40(%rsp)   ; options.BucketId
1405d915d  call *0xbc207d(%rip)      ; -> EOS_Lobby_CreateLobby (IAT 0x14119b1e0)
```

The field offsets match the EOS 1.x layout exactly (`ApiVersion +0`,
`LocalUserId +8`, `MaxLobbyMembers +0x10`, `PermissionLevel +0x14`,
`bPresenceEnabled +0x18`, `bAllowInvites +0x1c`, `BucketId +0x20`), and the
bucket string is `"Nioh"` (`0x1415864EC`). The requested service limit comes
from an object's `+4` field rather than an immediate at each call site.
One API hook covers all six service calls, but does not establish that
game-side arrays or mission state use that enlarged service limit.

The six sites sit in six ~0x180-byte wrappers, one of them a
`std::_Func_impl_no_alloc<lambda_...>` - the lambda bodies 6.3 described:

| site | wrapper function |
|---|---|
| `0x1405d3dbd` | `0x1405d3cc0` |
| `0x1405d75be` | `0x1405d74c0` (`std::_Func_impl_no_alloc<lambda_44d22d62...>`) |
| `0x1405d85dd` | `0x1405d84e0` |
| `0x1405d915d` | `0x1405d9060` |
| `0x1405d9bfd` | `0x1405d9b00` |
| `0x1405de0a0` | `0x1405ddfb0` |

**`CSessionP2pOpenCreateParam::SetParam` is vtable slot 2.** Its body at
`0x1405F0FF0` copies the requested member limit and separately deep-copies
metadata attributes. The earlier interpretation of its descriptor loop as
member allocation was incorrect:

```
1405f101b  mov 0x4(%r8),%eax ; mov %eax,0x20(%rcx)      ; this->+0x20 = src+4
1405f1022  movzbl 0x18(%r8),%eax ; mov %al,0x24(%rcx)   ; flags
1405f1032  mov 0x20(%r8),%eax ; mov %eax,0x28(%rcx)
1405f1039  mov 0x8(%r8),%eax ; mov %eax,0x30(%rcx)      ; attribute count <- src+8
1405f1042  cmp %esi,0x8(%r8) / jle                      ; loop over attributes
1405f1060  descriptor stride 0x20: id, type, size/value, buffer
1405f10b1  movslq global,%rax ; cmp $0x3,%eax ; jge skip ; allocator index bound
1405f1106  fields +0xc/+0x10 take positive inputs or defaults 0x400/0x100
1405f1126  movl $0x5dc,0x14(%rbp)                       ; constant 1500
1405f112d  movl $0x3,0x18(%rbp)                         ; constant 3, not max members
```

This code establishes an attribute descriptor table, not a structurally
three-slot member table. Native member-array sizing is in the create worker
at `0x1406049DD`: it reads the request's `+0x20`, allocates the primary array,
and allocates an additional `limit-2` array when the limit exceeds 2.
Those allocations happen before `ISteamMatchmaking::CreateLobby`, so a
service-only clamp is too late to resize the host.

## 7. Prior art (checked 2026-10-16)

| Project / source | Target | Relevance to this |
|---|---|---|
| Nexus "Nioh" (game 2755), 34 mods | Nioh 1 | saves, ADRENALINE MODE, ReShade, gore fix, costumes - **no player-count, netcode or lobby work at all** |
| FearLess Cheat Engine table, Nioh 1 | Nioh 1 | runtime memory editing, currencies only |
| `kassent/NIOH3-Plugin-Loader`, `Spring297/NIOH3-Plugin-Loader-Proton-Fix` | Nioh 3 | dinput8 proxy + `plugins/` dir - the same load path this mod uses, for a different game |
| "Nioh 2 DLL Loader", Nioh 3 Mod Manager | Nioh 2/3 | dinput8 based mod loading; **nothing equivalent exists for Nioh 1** |
| Ready or Not "More Players - 16 Player Co-op" | UE game | **host-only script raises the EOS/Steam lobby limit, all players need the data patch** - same two-layer shape as section 8 |
| Outward "Multiplayer Limit Remover", PEAK "More Players", Far Far West "Bigger Lobbies" | various | raising a lobby cap host-side is an established technique |
| Epic EOS documentation | platform | lobbies support **64** members per lobby - the ceiling in Nioh is the game, not EOS |
| TitanUnpacker, gust_tools, kvs-tools, RDBExplorer, QuickBMS scripts | other Koei/Gust titles | no tool found for Nioh 1's `K300` containers; Nioh 3's archives (`*.rdb/*.fdata/archive_xx.bin`) are a different, better-documented format |

So: **nobody has published this for Nioh 1.** No player-count mod, no netcode
reverse engineering, not even a plugin loader for the first game. What exists for
the sequels is only the loading mechanism (a `dinput8.dll` proxy), which is the
same approach used here, and the cross-game pattern that a bigger lobby needs the
host alone while the gameplay side must match on every client.

On the data side we identified Nioh 1's containers ourselves (no public spec):
`archive_XX.lnk` starts with magic **`K300`** followed by a count and per-archive
records, and each archive is described by `lfm_order_XX.bin` (magic **`LFMO`**,
version 1) which lists the original build paths, e.g.
`E:\workspace\shared\Nioh\application\data\DX11_data\archive\archive_06.cpp`,
plus a SHA-256-style hash per entry. Payloads open with their own markers
(`3SPK1000`, timestamps), i.e. they are compressed streams - a full extractor is
its own project and not on the critical path for multiplayer.

## 8. What a working third player needed, and what is still open

A bigger lobby alone did not establish a playable third participant. Status of
each game-side check after the 2026-10-20 runs:

1. **Session/member bookkeeping** — fixed: the host allocates three native
   members from its request, and the visitor allowance admits a second visitor
   ("Host visitor allowance").
2. **Mission sync** — joins, independent movement and a replicated attack were
   observed with three participants. Death/revival, boss fights and mission
   completion were not exercised.
3. **Player slots** — all three spawn and render on every client, with correct
   per-client slot tables. Targeting, loot ownership and rewards are unverified.
4. **UI/menus** — the party HUD names only one partner; otherwise admission
   and play work through the normal menus.
5. **Save/profile** — three-player mission records have not been validated.

The fix came from tracing the actual rejection, not from patching constants
that equal two; keep that method for the remaining items.

## 9. Realistic paths forward

1. **Ship the third player through the lobby, then patch the session layer
   incrementally** (what this repo is set up for): use the dump + log to find the
   2-player assumptions, patch them in memory from the proxy, test with 3 clients.
   Highest fidelity, highest effort. Next concrete step: hook the
   `CSessionP2pMember`-related allocation and the `CPacketModifySessionInfo`
   path in the dumped image, starting from the six `EOS_Lobby_CreateLobby`
   wrapper call sites and the session object whose `+4` field feeds
   `MaxLobbyMembers`.
2. **Verify how far the lobby clamp alone gets** before touching code: run the
   game with the mod, create "Yokai Realm with a Companion", and see whether a
   third client can even be invited/accepted. If the game already tolerates a
   third member at the lobby level, the patch surface shrinks a lot. This needs
   three accounts and is the cheapest experiment worth doing.
3. **Data-level option** - the `K300` containers are readable now (section 6.2,
   `tools/lnk.py`): 13,382 entries over 20 archives, with the script archives
   carrying the mission logic and its 1,072-name engine API. If the player count
   is a table or script value anywhere, that is far safer than code patching;
   the containers have no name table, so the next step is mapping the loader's
   `%s%s_%02d.lnk` prefixes (encrypted section) or identifying entries by
   content.
4. **Buy the sequel instead** - Nioh 2: Complete Edition (Steam 1325200) and
   Nioh 3 both support three-player co-op natively. Nothing in the Nioh 1
   codebase was designed for it. (Neither is in this Steam library today.)

## 10. Reproducing the measurements

```sh
./build.sh                       # build the proxy
./build.sh --harness             # build the fake EOS/steam DLLs + harness
tools/quiet.sh proxy             # harness run: offscreen, capped, no game needed
tools/quiet.sh game              # copy of nioh.exe under Proton (needs Steam launch)
tools/lnk.py list                # validate + inventory all 20 K300 archives
tools/lnk.py scan  <archive>     # payload signatures and entropy per entry
tools/lnk.py dump  <archive> <n> # extract one entry
python3 tools/symtab.py stats    # symbol names from the readable sections
python3 tools/symtab.py find CSessionP2p
python3 tools/analyze.py         # IAT/EOS reference map from the on-disk exe
python3 tools/rtti.py            # RTTI -> vtables -> methods (needs an image dump)
tools/sites.py                   # classmap -> nioh3p.sites.txt (call-site names for the proxy)
tools/sites.py --probe 0x5ef800  # what name would a caller at this RVA get?
python3 tools/logtriage.py       # grouped timeline of a proxy log
python3 tools/logtriage.py --decode          # decode every ktol_*/binary_* base64 value
python3 tools/logtriage.py --watch           # follow a live session, interesting lines only
tools/ktol_refs.py xrefs ktol_session_lock   # re-run the ktol_* xref analysis (RE_KTOL_KEYS.md)
```

Static facts in sections 1-3 come from `tools/pe.py` (PE/section/import/export
tables) and `tools/analyze.py` (`.pdata`-bounded disassembly). Section 5's
results are the actual log output of the two smoke tests.

## 11. Call-site naming, the online layer's class map, and the discovery gate

Everything in this section was established offline, with no game session.

### 11.1 The proxy now names what it sees

`tools/sites.py` turns the RTTI class map into `nioh3p.sites.txt`
(11,414 method addresses + 2,324 vtables, class-qualified), which the proxy
loads from its own directory and uses two ways:

* call sites: nearest entry at or below the caller's RVA, logged as
  `site: JoinLobby from CSessionP2pOpen::vfn2+0x3a`
* callback objects: exact vtable match, logged as
  `steam cb: RegisterCallback id=333 ... class CCallback<CSessionP2pSteam, GameLobbyJoinRequested_t>`

It is optional (missing file ⇒ `exe+0x...`) and installed by `install.sh`
(`cp` is replaced by write-and-rename, so reinstalling cannot corrupt a running
game). Verified in the harness: sidecar load, nearest-below resolution down to
`+0xc8`, and class resolution for a second interface type.

### 11.2 The online layer, by class (verified from the live log's own backtraces)

| class | role |
|---|---|
| `CSessionP2pSteam` | owns the Steam matchmaking surface; `vfn2`/`vfn3` drive session open/create; owns the **invite** handler |
| `CSessionCreate`, `CSessionCreateHandle` | hosting a session; owns `LobbyDataUpdate` handling |
| `CSessionP2pOpen`, `CSessionP2pOpenHandle` | joining/opening a session |
| `CSessionP2pHandle`, `CSessionP2pInfo` | shared session bookkeeping / lobby data record |
| `CSessionP2pPacketHandler` | `LobbyChatMsg` (P2P packet channel) |
| `CSessionP2pLockHandle` | writes the lock/unlock keys, registers `LobbyDataUpdate` |
| `SteamFriend::CSteamFriend`, `CSteamStats`, `Trophy::CSteamAchievements` | friend persona, stats, achievements |

Registered Steam callbacks in a live session: **333** →
`CCallback<CSessionP2pSteam, GameLobbyJoinRequested_t>`, **505** →
`CCallback<CSessionP2pSteam, LobbyDataUpdate_t>`, **507** →
`CCallback<CSessionP2pPacketHandler, LobbyChatMsg_t>`, **304** →
`CCallback<SteamFriend::CSteamFriend, PersonaStateChange_t>`,
**331** → `GameOverlayActivated_t`, 1101/1102/1103 → stats. There is **no**
rich-presence-join handler: invites arrive only as `GameLobbyJoinRequested_t`.

### 11.3 Why a hosted session is invisible to the client's search

`docs/RE_KTOL_KEYS.md` (offline xref analysis of the runtime image) found the
search filter:

```c
/* CSessionSearchHandle helper 0x609EA0, call at 0x14060A14E */
AddRequestLobbyListStringFilter("ktol_lock_status", "ktol_session_unlock", 0);
```

The EOS path does the same with `EOS_LobbySearch_SetParameter` (`ktol_lobby_lock_status`
== `ktol_lobby_unlock`, call at 0x1405DBC27). Consequences:

* **A locked session never appears in the search results** — the client cannot
  see it, so it never gets to join. This is the observed symptom: "he started
  hosting and my search does not detect it".
* Neither `EOS_Lobby_JoinLobby` nor `CPacketJoinRequest` tests the lock, and no
  `GetLobbyData` of the lock keys exists anywhere: the lock gates *discovery*,
  not the join.
* Lock state is one global byte `0x1417F9840` (setter `0x5EAF60`, emitters
  `CSessionP2pLockHandle` `0x5FB560`/`0x5FB810`).

### 11.4 New option: `drop_lock_filter`

`nioh3p.ini` → `drop_lock_filter=1` makes the proxy swallow the lock filter in
the search: `AddRequestLobbyListStringFilter` with a `lock_status` key is logged
as `DROPPED` and not forwarded, and the EOS `EOS_LobbySearch_SetParameter` with
a `lock_status` parameter returns success without setting it. Clients then see
locked sessions. Default is `0` (stock behaviour).

Verified in the harness: option off ⇒ the filter reaches the fake service
(1 call); option on ⇒ **0** calls, and the log shows
`Steam lobby filter: "ktol_lock_status" = "ktol_session_unlock" DROPPED (drop_lock_filter=1)`.

### 11.5 What else the analysis settled

* **Join-time validation** exists at `0x5F2010`: it builds `integer_<n>` keys,
  reads them with `GetLobbyData`, decodes and `memcmp`s them against the
  expected record, then compares the lobby's `ktol_owner_id` against
  `GetLobbyOwner`. The mod already watches both calls (slots 19 and 35), so the
  next session's log shows this handshake and where it stops.
* **Client capacity is service-driven; host capacity is request-driven.**
  `0x603C80` sizes client arrays from `GetLobbyMemberLimit`, with an extra
  `limit-2` array. The create worker instead reads `request+0x20` at
  `0x6049DD`, before `CreateLobby`. Live reads confirmed native capacity 2
  and no extra array with only the service clamp, then capacity 3 and an
  allocated extra array after the request hook.
* **EOS hook gap**: `EOS_LobbyModification_SetMaxMembers` and
  `EOS_LobbySearch_SetParameter` resolve from the EOS SDK but are "not usable in
  the import table (present=1)" and the writable-memory pointer scan finds
  nothing, so those two hooks are written but not armed. `docs/RE_EOS_CALLSITES.md`
  covers the exact slot to patch.

### 11.6 Instruments added this session

Lobby-list search (`RequestLobbyList`, every `AddRequestLobbyList*` filter,
`GetLobbyByIndex`), friends (`GetFriendCount`/`ByIndex`/`GamePlayed`/persona
state/rich presence + keys), legacy P2P (`SendP2PPacket`, `IsP2PPacketAvailable`,
`ReadP2PPacket`, `Accept`/`CloseP2PSessionWithUser`, `GetP2PSessionState`),
sockets (`ConnectP2P`, `CreateListenSocketP2P`, `SendMessageToConnection`,
`CloseConnection`), invites and per-member data, callback payload logging, and
the call-site/class naming of 11.1. Each was exercised in the harness before
install.

## 12. Static session-open and invite-path analysis

The only entry into a Steam session open is `SessionOpen_Start` (`0x5E9440`): it
requires `[CSessionP2pSteam+8] & 2`, no error bits, and `manager->vfn4() == NULL`,
then calls `0x5F71F0`. That function writes the request's **mode** dword
(`0x5F72B4` / `0x5F7328`) from the descriptor's kind at `0x5F7274` —
**0 = create, 1 = join** — and attaches the static `CSessionP2pOpen` object
(`0x1417F96C0`) through `CSessionP2pHandle::vfn3` (`0x5EFB50`).

`CSessionP2pOpenHandle::vfn2` (`0x5F6B20`) branches on that mode:

| mode | path |
|---|---|
| 0 (create) | embedded `CSessionCreate` (+0x20) → `CreateLobby` at `0x604CFA` |
| 1 (join) | embedded `CSessionJoin` (+0x48) → `CSessionJoinHandle::vfn2` state 1 → **`JoinLobby` at `0x6038F2`** |

Those two sites (`0x6038F2`, plus the caller-less `0x603F03`) are the *only*
`JoinLobby` callers in the image, and both require mode 1.

The inspected callback 333 handler (`0x5EF7E0`) stores the lobby id at `+0x260`
and calls `RequestLobbyData` (`0x5EF80E`). The mapped `LobbyDataUpdate` handler
(`0x5EF820`) sets `+0x254` to 2 or 3; the inspected pump path at `0x5EF03B`
produces error `0x80C81010`. This is not proof that stock invites can never join:
the earlier live traces used broken callback and SteamID-return hooks (§14).
A clean baseline is required before attributing “Connecting...” to game logic.

The mapped `ktol_invite_data` references (`0x5F0418` read, `0x6051A3` write)
do not establish a capacity gate. Two corrections to the 11.2 backtrace reading:
`0x604CE3` is inside the create worker (`0x604940`–`0x604F14`, its
`SteamInternal_ContextInit` call) and `0x5EF800` is inside the invite handler
(`0x5EF7E0`); that frame was a stack-walk artefact, not a poll chain.

### 12.1 Located lobby filters and capacity inputs

| gate | where | option |
|---|---|---|
| locked sessions hidden from the search | `AddRequestLobbyListStringFilter("ktol_lock_status", "ktol_session_unlock")` at `0x60A151`; EOS twin `EOS_LobbySearch_SetParameter` at `0x5DBC27` | `drop_lock_filter=1` (client) or `force_unlock=1` (host) |
| full sessions hidden from the search | `AddRequestLobbyListFilterSlotsAvailable` at `0x60A158` | `drop_slots_filter=1` (client) |
| host lobby capacity | `CreateLobby` `cMaxMembers` (Steam vtable slot 13) / EOS `CreateLobby` options+0x10 | `max_players=3` |

The inspected search route reaches `SessionOpen_Start` → mode 1 → `JoinLobby`.
Raising capacity and removing these filters does not establish that a third
member can complete that route or participate in mission gameplay.

### 12.2 Located EOS capacity calls

`EOS_LobbyModification_SetMaxMembers` has **no IAT slot and zero references** in
the image - the game never calls it, so the "not hooked" line was not a gap. The
only EOS capacity input is `EOS_Lobby_CreateLobby`'s `MaxLobbyMembers` (all six
call sites, slot `0x14119b1e0`), which is already clamped.

Two structural fixes came out of it: `install_hook()` now falls back to finding
an import slot **by value** (the writable-only pointer scan could never see an
IAT slot, since IAT pages are read-only), and `build.sh --harness` regenerates
the import libraries every run - a stale `libeossdk.a` silently hid new `.def`
entries.

## 13. EOS ABI and ownership corrections (2026-10-18)

The previous proxy itself could corrupt the join path:

* `EOS_LobbyDetails_CopyInfo` released the SDK result before returning it to the
  game. The detour now leaves ownership with the caller.
* `EOS_LobbyDetails_GetMemberCount` returns the count directly; it has no output
  pointer. The old detour interpreted an unspecified third argument as a pointer
  on the zero-count path.
* `EOS_Lobby_AttributeData` stores its value union **before** `ValueType`.
  AddAttribute options contain one attribute pointer, not separate key/value
  pointers. Search options similarly contain an attribute pointer, not a string.
* `CreateLobby` and `JoinLobby` return void. AddAttribute and SetParameter are
  synchronous two-argument calls, not callback-style calls.

Contracts: [attribute data](https://dev.epicgames.com/docs/api-ref/structs/eos-lobby-attribute-data),
[attribute options](https://dev.epicgames.com/docs/api-ref/structs/eos-lobby-modification-add-attribute-options),
[search options](https://dev.epicgames.com/docs/api-ref/structs/eos-lobby-search-set-parameter-options),
[member count](https://dev.epicgames.com/docs/api-ref/functions/eos-lobby-details-get-member-count).

Executed under Proton:

* SDK-shaped harness against the old proxy: exit 5 after JoinLobby, before the
  member-count result. Same harness after the fixes: exit 0, count 0 for a null
  handle, attribute call and remaining harness completed.
* Protected-page CopyInfo lifetime regression: old DLL exits 3 with
  `FAIL: CopyInfo result was released before its caller consumed it`; corrected
  DLL exits 0. Reproduce with `./build.sh --harness` followed by
  `DISPLAY=:9 tools/smoketest.sh ownership`.

These are local ABI/lifetime checks, not a successful online join.

Additional static check: `CPacketModifySessionInfo::vfn1` at `0x14060C1B0`
serializes a single byte from object offset `+0x14`, guarded by output-buffer
capacity. `CPacketJoinRequest::vfn1` at `0x14060CB50` serializes a byte, a field
through `0x1405FC530`, and a length-prefixed string. Neither inspected serializer
proves a two-member array cap; their packet consumers and mission behavior still
need tracing.

## 14. Steam ABI corrections and rendered game smoke (2026-10-18)

The shipped game and `steam_api64.dll` exposed additional proxy bugs:

* Callback vtable slot 0 is `Run(payload, ioFailure, SteamAPICall_t)`, not the
  two-argument form including `this`. The game function at `0x1406044E0`
  compares `r9` with the pending handle at object `+0x10`. Dropping that argument
  prevents matching async results from reaching the handler. The detour now
  preserves the handle and I/O-failure flag.
* `SetLobbyMemberLimit` takes a 64-bit lobby ID before the member limit. The old
  detour treated part of that ID as the limit and lost the actual limit.
* Virtual methods returning `CSteamID` receive a caller-owned result buffer after
  `this`. The flat exports marshal it and dereference the returned pointer.
  This is visible at shipped DLL RVAs `0x2180` (lobby by index), `0x21C0`
  (member by index), `0x21E0` (owner), and `0x1D30` (friend by index).
  All four detours now preserve that buffer and the shifted arguments.
* The wrapper decoder now handles `call [rax+disp8]` as well as `disp32`.
  Resolved vtable slot zero is represented separately from an unresolved hook.

The corrected fixture uses those marshaling instructions, rather than pretending
that the flat and virtual signatures are identical. With the pre-fix DLL, its
async-result regression exits 4:
`FAIL: matching call result or I/O failure was lost`.
With the corrected DLL, `tools/smoketest.sh proxy` exits 0: a matching error
result completes once, mismatched/duplicate results do not consume another
completion, the requested lobby gets limit 3, and full 64-bit lobby/member/friend
identities survive dispatch. `tools/smoketest.sh ownership` still exits 0.
These fixtures are not an online Steam service.

The actual game also reached its opening movie and interactive first-run setup
in the isolated rig. The previous `DXVK_WSI_DRIVER=xcb` setting caused
`Failed to initialize WSI`; removing it exposed Xvfb's Vulkan surface failure.
Headless gamescope on the iGPU provides a working surface while Steam's UI stays
on Xvfb. See `RIG.md` for the corrected launch path. A rendered game is not a
successful multiplayer join; concurrent licensed access remains blocked.

The final build was installed into slot a and relaunched through the persistent
`quiet-launch.sh` Steam launch option. The real game rendered and delivered a
`GlobalStatsReceived_t` call result at 14:08:37 with its 64-bit handle and
`ioFailure=0` visible in `logs-a/nioh3p.log`. This is a live callback smoke, not a
lobby result. The same verified DLL was then installed into the main Nioh
installation; its existing INI settings were preserved.
`~/nioh3p-test/shots/a-title-ready.png` captures the corrected build at Nioh
1.24.08's “PRESS ANY BUTTON” title screen after completing first-run setup.

## Rig boot investigation: corrected evidence (2026-10-20)

Earlier notes attributed slot b's missing game process to a clean return from
`WinMain`, an emulator-client kick, or a file-deleting agent. Those causes were
not established. Silent hooks on the game's `ExitProcess` and `TerminateProcess`
imports cannot observe every termination path, and a surviving Steam UI is not
evidence that `nioh.exe` is loading.

A fresh direct-launch trace used the existing slot-b launcher with:

```sh
WINEDEBUG=+timestamp,+pid,+tid,+process \
  bash "$HOME/nioh3p-test/scripts/run-b.sh"
```

The first fresh run's `steam-485510.log` recorded this sequence (later runs
overwrite that log):

| monotonic timestamp | Wine PID | observed event |
|---|---|---|
| 78402.350 | 0134 | `nioh.exe` created |
| 78402.588 | 0134 | requests `C:\Program Files (x86)\Steam\steam.exe steam://run/485510//` |
| 78402.592 | 0134 | `NtTerminateProcess` on itself, exit code 53 |

This run ends the game about 242 ms after creation, not after a multi-minute
loading phase. It does not prove which game/stub instruction requests the
relaunch, or what every older run did. The compositor subsequently displayed
Steam's Big Picture UI, captured in `shots/resumed-b-baseline.png`.
Inspection of live process environments placed that Linux Steam client under
`home-b/.local/share/Steam`, not the user's real Steam root. `xalia.exe` was
also in slot b's prefix; its presence does not establish a launch-broker role.

The installed Steam API DLLs were compared byte-for-byte. Slot b differed from
slot a only in six bytes at file offset `0xbd150`: an earlier experimental
`SteamAPI_RestartAppIfNecessary` return-false patch. Slot b has been restored
from slot a's unpatched emulator DLL. The shipping proxy no longer suppresses
Steam's restart decision. Replacing a Steam API implementation is not proof
that this protected game executable can boot without its normal launcher.

With the cloned Linux Steam client already running, full `proton run` launches
subsequently reached Nioh on both slots. Slot a loaded the level-26 Kyushu save.
The updated proxy was installed in both `lan-*` copies and exercised in the
actual game. An intermediate `runinprefix` experiment instead used WineD3D
and faulted at `nioh.exe+0x52ffe9`; it is not an equivalent graphics launch.

The original slot-b compositor also logged `Unhandled event EI_EVENT_SYNC (91)`
and `Unhandled libei event!` during injected input. The game animated its title
but ignored input even when both X11 and Windows reported it as foreground.
SIGTERM had left several rig-owned compositors alive; their launch logs show
socket collisions and a fallback to `gamescope-1`. Those remaining processes
were identified by rig HOME and force-stopped. The cloned Steam client was
restarted on Xvfb `:10`, then the game in a fresh headless compositor. Slot b
then accepted input and reached its main menu (`shots/b-clean-title.png`).

### Live matchmaking hook proof

On slot a, Torii Gate → Yokai Realm with a Companion → With Conditions →
Mission → Kyushu → Wreathed in Flame → Search first queried lobbies, then
created one when none matched. Thus this screen can **host**, not merely visit.
The live trace records:

- `19:15:17.429`: `CreateLobby(type=2, maxMembers=2)` clamped to 3.
- `19:15:17.512`: successful `LobbyCreated_t` (`EResult=1`), lobby
  `0x018600001e4b44dd`.
- Published owner `Nioh-LAN-A`, SteamID `<steamid-private-1>`, and unlocked
  session metadata.

The game stayed at “Searching for a companion…” while owning that lobby.
This proves the emulator-compatible hooks work in the game and that the
creation request uses capacity 3; it does not prove a third actor can join or
play.

### Verified two-player LAN gameplay

Slot b resumed Wreathed in Flame and held `E` at the starting shrine to pray,
then selected Summon Visitor and made an Ochoko offering. Slot a used Torii
Gate → Random Encounters → Become a Visitor → Without Conditions → Search.

- B created lobby `109775241935911929` at `19:25:41.813`, requesting 2 members;
  the proxy forwarded 3.
- A's `GetLobbyMemberLimit` returned **3** at `19:25:46.668`, then it called
  `JoinLobby` at `19:25:46.685` and received its `LobbyEnter_t` result.
- Both clients rendered the two actual characters in the same mission, with
  the remote name and health bar visible: `shots/b-two-player-host.png` and
  `shots/a-two-player-visitor.png`.
- Moving A backward changed A's position in B's view, captured in
  `shots/b-visitor-movement-observed.png`. P2P packets were exchanged between
  distinct identities `<steamid-private-1>` (A) and `<steamid-private-2>` (B).

This is a gameplay join and movement-sync result, not merely emulator peer
discovery or a lobby callback. The third participant is not yet verified.

### Third-client search: first observed failure

Slot c boots from its separate prefix after supplying native Steam SDK-library
symlinks to the cloned client. The transferred save is accepted in-game:
`shots/c-save-list-explicit.png` shows level 26 / Kyushu, and C reaches the
online world map and Random Encounters visitor search.

At `19:40:15–17`, C's real search returned zero lobby results and displayed
“No suitable matches were found” (`shots/c-third-admission-attempt.png`).
The lock filter was already dropped. A and B were still together in gameplay
at this point (`shots/b-baseline-still-live.png`).

One concrete mismatch precedes any third-player `JoinLobby`:

- C requires `binary_84 == "MDE2NgEBAQEBATIwAA=="`.
- B originally advertised exactly that value when creating its lobby.
- B's subsequent `CSessionP2pModifySessionInfoHandle` updates publish
  different values, including `"Vm0weE1GbFdXWGhXVw=="` at `19:40:11.373`,
  immediately before C searches.
- The game-side encoder call at `0x1405FFE1D` receives ASCII Base64 text as
  its 13-byte input during a live debugger stop, rather than the original
  binary compatibility data. This established the changed input before the
  Steam API boundary; the ownership cause was subsequently identified below.

That zero-result search was not evidence of Steam reporting a full lobby.
It did not reach third-player admission; the later native-admission proof
below is a separate result.

### Native host allocation and metadata ownership fixes

The API clamp ran too late for the host's own arrays. A live host advertised
three Steam slots while `CSessionP2pSteam+0x198` remained 2 and its extra-member
array at `+0x1a8` was null. The create worker reads request `+0x20` at
`0x1406049DD` and allocates the arrays before calling Steam at `0x140604CFA`.
Hooking `CSessionP2pOpenCreateParam::SetParam` before that worker produced
native capacity 3 and a non-null extra-member array; A still joined gameplay.

The metadata failure was descriptor self-assignment, not a compatibility
filter to remove. A bounded debugger capture showed the input descriptor and
destination descriptor were the same object. The publisher freed its old
binary buffer at `0x1405FFEDC`, stored the replacement pointer into that same
descriptor, then copied from the now-replaced source pointer at
`0x1405FFF5E`: effectively copying the uninitialized replacement to itself.
The proxy's guarded continuation at `0x1405FFE90` skips only that redundant
local replacement for exact descriptor self-assignment. It retains network
and Steam publication and the original non-alias path. The live host installed
the guard at `20:46:06.174`; a late C search at `20:47:44.785` read the original
canonical `binary_84` value and reached `JoinLobby`.

### Three native members accepted

Bounded Win32 debugger probes on C then established more than a lobby callback:

- C's native array capacity was 3 before response execution.
- `CPacketJoinResponse::Execute` at `0x14060B090` received status 1, local index
  2, owner index 0, ten metadata attributes and three member records.
- The records at response `+0x438`, stride `0xa8`, held distinct B/A/C IDs
  `<steamid-private-2>`, `<steamid-private-1>`, `76561198023713187` and their correct
  `Nioh-LAN-B/A/C` names. Response `+0x2c` counts metadata descriptors;
  `+0x430` counts members. `+0x4cc` is a field within the first member, not
  the start of the member array.
- At `0x14060B38C`, the executor wrote latch value 1 to
  `[manager+0x190]->+0x18->+0x4e4`. The native join handle maps 1 to success.
- A separate normal-close operation followed with the latch still 1. Its
  public native path was captured through `0x1405F4EE0`, with game-side
  close wrapper return `0x140D71DCC` and higher caller `0x140D64246`.

This was native three-member admission, not yet mission gameplay; the next
sections trace the game-side rejection and the fix.

An all-thread hardware watchpoint on `0x141B54795` narrowed that decision
without guessing a player-count constant. On a fresh C process, its first
write to 1 came from the unconditional setter `0x140D59AB0`, called at
`0x140D7A743`, after native capacity 3, own index 2 and join latch 1 were
already present. The task pump cleared the byte at `0x140D5F416` 94 ms later.
Further bounded probes separated cleanup from its cause. At `0x140D7A34B`,
the host-connection helper returned `AL=1` with native own index 2, capacity 3
and join latch 1, but operation `+0x3cc` had become 1. The final all-thread
watch captured its writer at `0x140D6AF52` (next RIP `0x140D6AF59`):
sender index 0 (B), raw packet ID 7, payload DWORD 2. C then follows the
normal cleanup path. This is a host-originated game-protocol rejection,
not a failed host-connection predicate or native lobby-capacity failure.

The packet dispatcher uses **two-stage mapping**, not an ordinal switch:
`rawID - 1` indexes byte table `0x140D6C2E8`, which indexes target table
`0x140D6C228`. Raw ID 7 reaches `0x140D6AE0A`; payload byte 2 reaches
`0x140D6AF3F`. Earlier case-ordinal labels must not be treated as raw IDs.
The host probe for the incorrectly labelled ID-5 handler timed out, restored
its breakpoint and detached; that negative result says nothing about raw ID 7.

The visitor's join request is raw ID **16** (sent at `0x140D7A68F` with
`ecx = 4 + 0xc`), handled by the host at `0x140D6B3DA`; `0x140D6B327` is the
tail of the raw-12 handler, not the admission handler. `0x140D6B4B2–0x140D6B4C7`
sends raw ID 7, reason byte 2, four payload bytes, to the incoming sender:

- Operation `+0x3c9` must be zero; the sender slot's `+0x104` flags must lack
  masks `0x8` and `0x4`.
- Request byte 2 is stored into sender slot `+0x10b`. Nonzero selects
  `root[+0x39d] - D7A1E0(root)`; zero selects
  `root[+0x39c] - D79E50(root)`.
- Both counters scan non-host slots 1–3, requiring slot `+0xe8 == 2` and
  flag `0x4`; they partition slots by nonzero versus zero `+0x10b`.
- When DWORD `0x14191DE34 != 2`, signed delta `<= 0` selects reason 2.
  For value 2, the alternate `D8673F0` check selects that response instead.

### Host visitor allowance: the rejecting branch, and the fix

A read-only 10 ms poller on B (no debugger) captured the decision inputs:

| moment | `root+0x39c` | `0x14191DE34` | `0x14191DE38` | slot 1 (A) | request byte |
|---|---|---|---|---|---|
| boot (root reset `0x140D7AC0C`) | 1 | 0 | 0 | – | – |
| shrine offering (`0x140D7BB77`) | 1 | 0 | 0 | – | – |
| A admitted | **0** | 0 | 0 | `e8=2 f=04 t=00` | 0 |

`root+0x39c` is the number of ordinary visitors the host may still admit. A's
request computed `1 - 0 = 1`; because `0x14191DE38 == 0`, the accept path then
cleared the consumed allowance (`0x140D6B539`). C's request computed
`0 - 1 = -1` and took the raw-7 reason-2 reject. Writing `root+0x39c = 2` into the
live host (diagnostic only) let C's next request through (`e8=2 f=04`), and C
entered the mission.

The game already contains a two-visitor value: its own shrine setter
`0x140D5FB30` accepts values below 3, and an item path (`0x140CFA480`, item
`0x74CC`) raises 1 to 2. The proxy now changes the single-visitor constant at
every store that opens recruitment — root reset `0x140D7AC0C`, shrine offering
`0x140D7BB77`, controller reset `0x140D7CA03`, start-recruiting `0x140D7DAA8`,
and the online shrine setter argument `0x140CF9F36` — to `max_players - 1`
(setter argument capped at 2 by the game's own `< 3` check). Consumption,
departure (`0x140D7B234`) and close paths are untouched. Installation waits for
the exact original instruction bytes after `.text` decryption and logs
`game visitor allowance: recruitment opens 2 visitor slot(s)`.

### Three players in one mission (2026-10-20)

With the allowance build on all three slots, cold-launched, no memory writes:
B offered at the Wreathed in Flame starting shrine (allowance 2), A searched and
was admitted (allowance stays 2: `2 - 0 = 2` is not the last slot), then C
searched and was admitted (`2 - 1 = 1`, which consumes the allowance to 0).
All three rendered each other in the mission, and each moved independently;
the other two clients showed the movement:
`shots/f-{b,a,c}-3p-1.png`, `shots/f-{b,a,c}-3p-move.png`. Each client's slot
table converged to three connected members (`e8=2`, flags `0x79`/`0x7d`).

A second clean run (all three slots cold-launched with the final
`154a5a1d…` build) repeated this with C joining first and A second, and added:

- C's light attack consumed C's Ki (104 → 93) and B rendered C mid-thrust
  while A stood apart: `shots/z-c-attack.png`, `shots/z-b-sees-c-attack.png`.
- Host slots for both visitors kept the admission flag (`f=7d` each).
- C then quit normally; B and A **stayed in co-op** (B's party HUD switched to
  `Nioh-LAN-A`): `shots/z-{b,a}-after-c-left.png`.

A third run used the player package (`dist/nioh3p-player.zip`, DLL
`b66d2958…`, `trace=0`). That build installs only the functional hooks, and no
P2P, friends or callback tracing. Host B plus A, then C, joined one mission
again, and B rendered both visitors moving (`shots/p-b-3p.png`,
`shots/p-b-3p-move.png`). The host's metadata guard installed when C joined,
exactly as in the research build. Each client's log was 3.6–3.7 KB for the whole
launch and grew 0 bytes over 2 minutes of three-player play. The research
build's host log grew ~2,000 lines/s (≈120 KB/s) in a mission, mostly
`SendP2PPacket`/`IsP2PPacketAvailable`/`ReadP2PPacket` lines written
synchronously on the game's network path.

#### Windows safety pass (player build `fd4a5824…`)

An adversarial Windows-safety audit (`agent://WindowsSafetyAudit`) found no
write to the game install, the save folder or the system: the only files are
`nioh3p.log`/`nioh3p.prev.log` (game folder, `%TEMP%` fallback), and every code
write was byte- or identity-checked. Fixes shipped from it:

- `nioh_launcher.exe` is x64 and also imports `DINPUT8.dll` (`objdump -p`), so
  the proxy ran there too, rotating the game log away. The proxy now acts only
  in `nioh.exe` (and the harness host `testgame.exe`); elsewhere it only
  forwards DirectInput.
- On any other `nioh.exe` build all EOS, Steam and game hooks are skipped
  (`unsupported nioh.exe build … mod inactive`); before, the EOS/Steam hooks
  installed regardless and their detours read hard-coded struct offsets.
- The metadata-guard patch is one `lock cmpxchg16b` of the 16-byte-aligned site
  instead of four compiler-generated stores, so no other thread can execute a
  torn instruction. The extra RAX/R10 writes in the guard were checked dead
  against the disassembly (`0x5ffea2` paths write RAX first; R10 is unread
  before the next call).
- A Steam vtable slot is patched only if its current value points to
  executable memory; `max_players` outside 0/2–4 becomes 3; `DllCanUnloadNow`
  always answers `S_FALSE`.

Verified: harness PASS at trace=0/1 plus the ownership test; the harness
renamed `nioh_launcher.exe` wrote no log; renamed `nioh.exe` (wrong identity)
logged the inactive line and its clamp test failed as intended;
`max_players=9` logged `out of range; using 3`. The rig repeated host B + A + C
in one mission with this build, and the host logged the guard install at C's
join. `nioh.exe` has no CFG (`DllCharacteristics 0x8160`) and imports only
`DirectInput8Create` from dinput8. Not testable here: Defender / Smart App
Control behaviour on an unsigned DLL (the player README covers the recovery:
delete `dinput8.dll`).

#### Rig artifact: second-visitor load stall

On this rig the visitor that joins second can sit on the loading screen for
~280–300 s (reproduced with C second and with A second); the first visitor loads
in ~20 s. Network admission is immediate (state 9), but the mission loader stays
in phase 1 (`*(0x1418715E0)+0x5D0`). A temporary script-binding call counter
(diagnostic build, since removed) showed the mission script polling only
`Load::IsLoadEnd` (`0x14089BF90`, resource-manager completion) at **1–5 calls
per 5 s**, i.e. about one frame per second. The integrated GPU (`1002:13c0`)
reported 100 % busy with the two in-mission clients. Pausing the first visitor's
process (`SIGSTOP`) for 8 s let the newcomer render: `Load::IsLoadEnd` jumped to
131 calls per 5 s and its load completed inside that window. In the clean run,
pausing C in 5 s bursts during A's join cut A's load to 33 s.

The stall is therefore GPU starvation from three game instances sharing one
iGPU, not a protocol wait. Its knock-on effects are also rig-only: the host times
the starved newcomer's slot out after ~90 s, reconnection clears that slot's
admission flag `0x04`, and when the first visitor later leaves, the host counts
no admitted ordinary visitor and ends co-op ("Co-Op Defeat" for the remaining
visitor). With the flag intact (clean run above), the first visitor's departure
leaves the other two in co-op. Ruled out along the way: the member-slot connect
gate at `0x140D794A0` (widening it to states 9–12 connected slot 1 early but did
not shorten the stall), the coop wait counters `N+0x15837C/0x158388` (zero on all
three clients) and the 18000-tick timer at `R+0x20` (frozen during the stall).

Other observations:

- The party HUD lists one partner name; the third player renders without a name
  plate on some clients.
- While a visitor is present the host has no Pray prompt at the shrine. After a
  departure the allowance stayed 0 (departure only decrements it), so a
  replacement visitor cannot be admitted until co-op ends; the rig's rejoin
  tests reopened it with a diagnostic memory write.
- Attaching a Win32 debugger (`DebugActiveProcess`) to an in-mission client made
  it exit silently (A, exit 0, no proxy exit hook). Use read-only
  `ReadProcessMemory` pollers and brief `SuspendThread` sampling instead.
- C's earlier `XI_BadDevice` startup exits did not recur: C cold-booted three
  times this session (once with the previously failing 61,952-byte build, twice
  with allowance builds).

### Launch-command and filesystem limits

Several archived compound commands used `pgrep -f 'lan-b'` or
`pkill -f 'gamescope --backend'` before copying files and launching. Those
patterns also match a `bash -c` command line containing the command itself.
They can kill the issuing shell before the copy or output-file creation runs.
A read-only demonstration reproduced this self-match. Such failures do not
establish that a different process later deleted or reverted the intended files.

The reviewed rig scripts contain no timed log deletion or restart supervisor.
`tools/rig/setup.sh` explicitly trashes the stock-control slot's proxy and INI
under `home-b/.local/share/Steam/steamapps/common/Nioh`; it does not install or
manage the separate `lan-a` and `lan-b` emulator directories. Other reported
file disappearances remain unattributed; no external cleaner has been identified.

The rig prefix is **inside** its Steam root:
`home-b/.local/share/Steam/steamapps/compatdata/485510/pfx`. Trashing that root
would trash its prefix and saves too. Btrfs reflinks do not change this ancestry.
Future cleanup must target verified rig-owned PIDs, not global Wine/Steam names
or command-line fragments that can match the cleanup shell.

Boot traces alone do not establish multiplayer; the two-player gameplay
evidence above is a separate check.
