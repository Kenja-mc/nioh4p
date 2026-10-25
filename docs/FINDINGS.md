# Findings: Nioh: Complete Edition, and what "three-player" requires

Everything below is measured on this machine against the installed Steam build.
Raw facts come first. The implications follow.

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

`nioh.exe` is PE32+ x64 with 8 sections. `SizeOfImage` is 0x306D718… (50,782,208 B
at runtime). The entry point is RVA 0x303B310, which is inside `.bind`, not
`.text`.

## 2. The game's code is encrypted at rest

| Section | Entropy (file) | Note |
|---|---|---|
| `.text` (18,449,408 B) | **7.997** | code, encrypted |
| `.rdata` | 5.06 | readable: import table, strings, RTTI |
| `.data` | 4.21 | readable |
| `.pdata` | 6.86 | valid function table (72,232 entries) |
| `.bind` | 7.955 | loader/DRM stub, entry point |

Consequences, all verified:

* Linear disassembly of `.text` desynchronizes. There are zero references to any
  imported function in it (18 MB of ciphertext).
* `.pdata` therefore points at ciphertext: 72,232 "functions" decode as garbage.
* Only the entry stub (`.bind`) is plaintext - it is the Steam DRM wrapper:
  `call $+5 / push rbx..r15 / mov rcx,[rsp+0x78] / sub rcx,5 / … / call`.
* Running `nioh.exe` outside a Steam context makes it exit immediately
  (exit code 53) before the section is decrypted.
* With `steam_appid.txt` next to a copied `nioh.exe` (Steam client running), the
  stub unpacks successfully, measured from inside the process:

```
decrypt-probe: .text delta=0.0%  runtime-entropy=7.997   (t+100 ms, still encrypted)
decrypt-probe: .text delta=99.6% runtime-entropy=5.923   (t+300 ms, plaintext)
```

No static patch of game code is possible. Any mod must act at runtime or on
data files. This is the single most important constraint.

## 3. Online transport

`nioh.exe` imports 124 `EOS_*` entry points plus the Steam API:

| Area | EOS APIs used |
|---|---|
| Identity | `EOS_Auth_*`, `EOS_Connect_*`, `EOS_Platform_*` |
| Session | `EOS_Lobby_CreateLobby`, `JoinLobby`, `LeaveLobby`, `UpdateLobby`, `DestroyLobby`, `KickMember`, `PromoteMember`, `SendInvite`, `*_AddNotify*` |
| Discovery | `EOS_LobbySearch_*`, `EOS_LobbyDetails_*` |
| Data | **`EOS_P2P_SendPacket` / `ReceivePacket` / `AcceptConnection`** |
| Extras | `EOS_Stats`, `EOS_Achievements`, `EOS_Leaderboards`, `EOS_Presence`, `EOS_Ecom`, `EOS_Friends`, `EOS_UI` |

The EOS SDK DLL itself is not encrypted. Its code entropy is 6.6, and it has 574
exports in `.text`. The game's import table is readable plaintext. The slot for
`EOS_Lobby_CreateLobby` sits at RVA 0x119B1E0 and holds the same address that
`GetProcAddress("EOS_Lobby_CreateLobby")` returns. The EOS boundary is therefore
the one place where the game can be intercepted without touching its code.

Client-side classes visible through RTTI (which lives in `.rdata`/`.data`,
unencrypted, and has 2,324 descriptors once the parser handles template names).
See `analysis/classmap.txt`:

* Koei's own online library, namespace `ktol`: `ktol::online_service::eos::basic::BasicService`,
  `ktol::sessionp2p::eos::session::SessionService` - i.e. `CSessionP2p*`,
  `CProtocol`, `CPacket*` (`Hello`, `JoinRequest`, `JoinResponse`, `Reject`,
  `Leave`, `Bye`, `Ping`, `Pong`, `ChangeOwner`, `GrantOwner`, `Lock`,
  `ModifySessionInfo`, `Reliable`, `Unreliable`, `Archive`), `CSessionP2pMember`,
  `CSessionP2pSessionInfo`, `CSessionP2pSearch`, `CNetManager`
* Both transports: `CSessionP2pProcessSteam`, `CSessionP2pSteam`,
  `CRawSocketSteam`, `CCallback/CCallResult<CSessionCreateHandle, LobbyCreated_t>`,
  `CCallResult<CSessionJoinHandle, LobbyEnter_t>`,
  `CCallResult<CSessionSearchHandle, LobbyMatchList_t>`,
  `CCallback<CSessionP2pSessionInfo, LobbyDataUpdate_t>`,
  `CCallback<CSessionP2pPreparationHandle, SteamRelayNetworkStatus_t>`
  (Steam matchmaking) and the EOS lobby/P2P path
* the Steam side is reached through `SteamInternal_FindOrCreateUserInterface`,
  not flat imports - the interface versions the game asks for are in the image:
  `SteamMatchMaking009`, `SteamNetworking006`, `SteamNetworkingSockets009`,
  `SteamNetworkingUtils003`, `SteamFriends017`, `SteamUser021`, `SteamUtils010`,
  `SteamController008`

Co-op runs over Steam matchmaking and P2P when crossplay is off. It runs over EOS
when crossplay is on. PCGamingWiki documents that online settings toggle. A mod
that only raises the EOS lobby cap covers only half the cases. Both paths are
hooked now (section 5).

The lobby calls themselves come from lambdas held in `std::function` objects. The
"6 call sites" are `std::_Func_impl_no_alloc<lambda_..., SessionService*>`
wrappers whose vtables sit together at `0x1401586150`-`0x1401586310`. Each
wrapper contains one CreateLobby lambda and one JoinLobby lambda. That is why
they share one code shape. It is also why the mode in use has to come from a live
run, not from the call sites alone.

## 4. What the co-op limit actually is

From the shipped manual (EN and JP, `launcher_data/pdf/manual_en.pdf`,
`manual_jp.pdf`), which is the authoritative statement:

* Summon Visitor (Ochoko Cup during a mission): "summon a Visitor - *another*
  player online who will join you as a co-op partner" - i.e. host + 1.
* Yokai Realm with a Companion (Torii Gates): the JP text says 「常世同行では
  プレイヤーが2人とも落命した状態になると…」 - "when both players have died" -
  i.e. exactly 2.

The shipped co-op modes expose two participants. That establishes the supported
design. It does not prove that every underlying session or actor array has two
slots.

## 5. What this mod does about it

Implemented and verified (`src/proxy/nioh3p_proxy.c`):

1. `dinput8.dll` proxy. `nioh.exe` imports `dinput8.dll` statically, so the
   loader maps the DLL before the DRM stub runs. Every real DirectInput export
   is forwarded to the system DLL. The build is freestanding with KERNEL32
   imports only, because Proton's Wine ships no `api-ms-win-crt-*` apisets.
2. Runtime image dumper. It waits until `.text` is decrypted, then writes the
   whole 50,782,208-byte runtime image (`nioh3p_image_boot.bin` / `_settled.bin`
   plus a JSON manifest). This is what makes the game's code analyzable at all.
   The netcode region of the decrypted image is `0x1405D3xxx`-`0x1405E4xxx`
   (EOS lobby/P2P call sites).
3. EOS lobby clamp. It patches the game's import-table slot for
   `EOS_Lobby_CreateLobby`, and for `EOS_LobbyModification_SetMaxMembers` when
   imported. It raises `MaxLobbyMembers` to `max_players` (default 3), logs the
   original value on every call and re-applies the patch if the game restores
   it. The game never calls `SetMaxMembers`, so `CreateLobby` is the only
   capacity lever.
4. Steam matchmaking clamp. The mod captures interface objects through the
   `SteamInternal_FindOrCreateUserInterface` import hook. It patches explicit
   vtable indices for the exact versions requested by this game:
   `SteamMatchMaking009`, `SteamFriends017`, `SteamNetworking006` and
   `SteamNetworkingSockets009`. Other version strings are forwarded without a
   hook table. `RE_STEAM_CALLSITES.md` §§2.1–2.2 maps the version evidence and
   the game-side calls.

   `cMaxMembers` is clamped like the EOS field. Interface lookups include their
   version string in the log. Matchmaking slots are re-applied if restored.
   Flat export instruction decoding is removed. The emulator's exports are real
   functions, not the shipped API DLL's small dispatch thunks, so their bytes
   cannot identify the requested interface layout reliably.

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

   `GetLobbyMemberByIndex` is also watched, at index 18. The MSVC virtual
   method's caller-owned SteamID return buffer is preserved. The compiled
   harness exercises unsupported-version forwarding before any supported
   lookup. It also exercises native supported-version lobby calls, full-width
   identities and asynchronous result delivery. The 2026-10-20 build and
   `tools/quiet.sh proxy` run completed with rc=0. These are local ABI and
   capacity checks, not a gameplay join.
5. Online instrumentation, used to find the game-side limits. It covers
   `EOS_Lobby_CreateLobby` and `EOS_Lobby_JoinLobby` with arguments and a stack
   backtrace. The backtrace is module-relative, so the caller can be read
   straight out of the image dump. It also covers
   `EOS_LobbyDetails_GetMemberCount`, which logs the count the game actually
   sees, and `EOS_LobbyModification_AddAttribute`, which logs the matchmaking
   attribute keys and values, the way modes are labeled. It covers
   `EOS_LobbyModification_SetMaxMembers` too. `RtlCaptureStackBackTrace` takes
   the backtraces, and ntdll resolves them at runtime.
6. Game-side session changes, on the exact supported executable only. Original
   bytes or vtable entries guard each change. The host's create-parameter member
   count is raised before native allocation. Exact descriptor self-assignment in
   the metadata publisher keeps its buffer. Recruitment opens `max_players - 1`
   visitor slots instead of one. Together these admitted a second visitor into a
   live mission. See "Three players in one mission".

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
real thing. `tools/testgame/fake_steam_thunks.S` holds the two thunks
byte-identical to the shipped DLL. `tools/testgame/fake_steamclient.c` is a
separate module holding the implementation class and its vtable, as
`steamclient64.dll` does. `fake_steam.c` only plays the interface factory.
Recording happens in the "steamclient" module, so the clamped value proves that
the proxy patched a vtable in another module. It proves this both through the
vtable and through the flat thunk. Neither Steam nor EOS nor three accounts are
involved.

## 6. Where the two-player cap sits in the code

From the decrypted image (`.pdata`-bounded disassembly of all 72,232 functions):

* `EOS_Lobby_CreateLobby` is referenced 6 times in 6 distinct call sites
  (`0x1405D3CC0`, `0x1405D74C0`, `0x1405D84E0`, `0x1405D9060`, `0x1405D9B00`,
  `0x1405DDFB0`), each with a matching `EOS_Lobby_JoinLobby` site ~0x260 bytes
  later - i.e. per-mode create/join paths. They share one compiler-generated
  shape: build the options struct on the stack, call through the import table,
  bump a counter. That is why they look like a generated wrapper layer. They are
  not called through a common dispatcher, so the mode in use has to be
  identified from a live run.
* The options struct built at `[rsp+0x20..0x40]` maps onto
  `EOS_Lobby_CreateLobbyOptions`: `LocalUserId` from an object field,
  `PermissionLevel` computed as 0 or 2, `bPresenceEnabled` from a flag,
  `BucketId` = the string `"Nioh"` (one bucket for every mode, so a bigger
  lobby stays discoverable), and
  `MaxLobbyMembers` copied verbatim from a session-object field (`[rbx+4]`).

* The game never imports `EOS_LobbyModification_SetMaxMembers` (checked in the
  import table), so capacity is decided once, at `CreateLobby`.

All struct layouts used by the mod are checked against Epic's own headers
(`eos_lobby_types.h`). This corrected one guess. In `EOS_LobbyDetails_Info`,
`AvailableSlots` sits at +28 and `MaxMembers` at +32. A naive layout guess reads
the wrong field. `EOS_LOBBY_MAX_LOBBY_MEMBERS` is 64, so EOS itself is
never the ceiling.

At the EOS boundary, the requested "2" is copied from session and matching state
(`COnlineSession`, the `CSessionP2p*` family, `Matching@Menu@@`). It is not
encoded as a constant in the call itself. The API clamp raises the service
request. It does not establish native game capacity. The Steam host path
allocates its member arrays before it reaches `CreateLobby`, so it also needs
the early create-parameter hook described in §6.1.

### 6.1 The session layer can already allow three

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

The constants at `self+0x0c`, `+0x10`, `+0x14` and `+0x18` include 1024, 256, 1500
and 3. Their presence does not establish a three-player limit. In particular,
`self+0x18 = 3` is not the member limit. The `cmp $3` near `0x1405F10B8` bounds
an allocator implementation index, not a player slot.

The decisive native-capacity evidence is the create worker. It allocates arrays
from `self+0x20` before it calls Steam. A live host initially had native
capacity 2 while Steam advertised 3. Clamping the request before allocation
produced native capacity 3 and a non-null extra-member array.

### 6.2 What the archive containers hold

The 60 files in `archive/` are 20 `K300` containers, holding 13,382 entries and
71.19 GB. The other 40 files are `archive_NN.lnk` companions of the C++ tooling,
plus unrelated files. The format comes from the files themselves and was
validated on all 20. Every entry is monotonic, 0x800-aligned and inside the
file. The last entry pads up to exactly the file size:

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

The script entries (06/08/15/18) start with a build stamp. The stamp is `0x18`
followed by the 24-byte ASCII `2011-04-29T14:26:48+0900`. Records of
`u16 length, u32 hash, name` follow. The names are the engine API that the
scripts can call (`umud::yield`, `Status::SetEnd`, `Object::GetType`, ...).
1,072 unique `Class::Method` names appear across the script archives
(`analysis/script_api.txt`). 377 of them face players or online play, including
`Online::IsPlayerHost`, `Character::GetSummonNum` and
`Chara::IsCoopPlayerEnterBox`. Mission scripts are online-aware by construction.
All 211 scripts in archive 15 reference `iScript::StopReceiveOnlineSyncNumber`,
and 208 reference `Script::WaitPlayerApproache`.

There is no name table in a container, so entries are addressed by index. The
loader composes names itself. The exe still carries the format strings
`%s%s_%02d.bin` and `%s%s_%02d.lnk`, which mean prefix plus shard number. A
plain `.bin` variant suggests per-file overrides, but the prefix table lives in
the encrypted section (unverified).

### 6.3 The symbol table, and the session classes it labels

The game ships a symbol table in the readable `.rdata`/`.data`, with 4,082
unique names. It covers the whole `ktol` online layer and the EOS/Steam surface
that layer sits on (`analysis/symbols.txt`). `analysis/session_symbols.txt`
holds 449 session and network names, extracted with `tools/symtab.py`. The table
needs no image dump, and it answers the "what shapes exist" question directly:

* host/guest/coop slots: `Online::IsPlayerHost`, `Online::IsPlayerGuest`,
  `Online::IsPlayerCoop`, `Online::IsPlayerCoop1`, `Online::IsPlayerCoop2`,
  `Online::IsPlayerTokoyoCoop`, `Online::GetExistPlayerNum`
* PvP on the *same* session stack: `Online::IsOnlinePlayVersus`,
  `Arena::IsMatchType1vs1`, `Arena::IsMatchType2vs2`, and the 1VS1/2VS2
  leaderboards and rankings - 2vs2 means the transport and the session layer
  already carry four members
* co-op partner handling is singular: `Online::GetCoopPartnerPlayerPosition`,
  `Online::IsCoopPartnerPlayerObjectExist`, `Online::IsPlayerCoop`, plus the
  mission-side flags `Mission::IsCooperating` / `Check::IsCooperating`
* summon slots: `Character::GetSummonSlotNum`, `Check::GetSummonSlotNum` (the
  visitor mechanism), `Character::GetSummonNum`
* the session classes themselves: `CSessionP2pOpenCreateParam::SetParam` /
  `::Cleanup`, `CSessionCreateHandle::OnLobbyCreate`,
  `CSessionP2pSessionInfo::OnLobbyDataUpdate`, `CSessionP2pModifySessionInfo::Update`,
  `CSessionSearchHandle::SearchSession`, `CSessionP2pCloseHandle::Terminate`

The RTTI pass adds `analysis/classmap.txt`, with 2,324 classes and their vtables
and method addresses. Together, the label pass names the session layer without
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
(`0x1405F0FF0`–`0x1405F11BA`). It writes the requested maximum members at
`self+0x20`. That field is separate from the constant 3 at `self+0x18`. The
latter is not evidence of a three-member request. The early request hook now
raises `self+0x20` before the Steam create worker allocates its native member
arrays.

The same `.rdata` blob gives a way to label methods mechanically. It keeps runs
of name strings next to runs of 8-byte method pointers. The 62-pointer run at
file offset `0x15856e0` contains exactly the four `CSessionP2pOpenCreateParam`
slots listed above.

Some facts still need a runtime image dump. They are all code-level: the `+4`
field of the session object that feeds `MaxLobbyMembers`, the
`CSessionP2pMember` array sizing, `CPacketModifySessionInfo`, and which of the
`CSessionP2pOpenCreateParam` slots is `SetParam`. `analysis/classmap.txt` and
`analysis/symbols.txt` survive from the earlier pass. The image dump itself does
not. It was written into the game directory and was deleted during the
uninstall. One Steam-launched run with `out_dir` set regenerates it outside the
install.

### 6.4 Out-of-view runs: the VM question, and what actually works

The development runs must not take over the desktop, so a subagent surveyed the
host for an invisible-run design. The result is no VM. A VM is the most
expensive option, and it does not remove the real blocker, the Steam DRM stub.
Measured:

| Option | Verdict | Evidence |
|---|---|---|
| VM + GPU passthrough | rejected: the only free GPU is the iGPU, and the 9070 XT is `boot_vga` carrying both monitors, so passing it through takes the desktop away | `cat /sys/bus/pci/devices/0000:03:00.0/boot_vga` = `1`; `card0` (iGPU) has no connectors; IOMMU groups clean (no ACS override needed) |
| VM + iGPU passthrough | possible but pointless: +qemu/libvirt/OVMF/swtpm installs, ~40 GB guest OS, a second 74 GiB game install inside the guest, and it still needs a second Steam account that owns 485510 | `pacman -Qs qemu` shows only `qemu-user-static`; no `virsh`; no images |
| harness runs | need no VM and no game at all - fake EOS/steam DLLs + `testgame.exe` under a throwaway prefix | `tools/quiet.sh proxy`, ~13 s |
| **headless gamescope** | **the answer**: renders a real Vulkan swapchain with zero windows | `env -u DISPLAY -u WAYLAND_DISPLAY gamescope --backend headless -W 640 -H 480 -r 30 -- vkcube --c 40` -> `Starting headless backend`, rc=0; `hyprctl clients | grep -c Window` = 4 before, 4 after |
| Xvfb | fine for the no-render harness, **useless for the game**: no DRI3, so Vulkan presentation dies | `xvfb-run -a vkcube` -> `vulkan: No DRI3 support detected - required for presentation`, then SIGSEGV |

`tools/quiet.sh game` therefore runs the game inside `gamescope --backend
headless`. It falls back to Xvfb only when gamescope is absent. The caps are
unchanged, and the same wrapper re-verified them inside the run
(`cpu.max=200000 100000`, `cpu.weight=20`, `memory.high=8G`). One practical
trap: a killed gamescope can leave children holding stdout. A wrapped run must
never be piped. The runner writes to a log file and uses
`timeout --foreground -k 10`, which keeps a headless run bounded.

For a run that Steam itself must start, `tools/quiet-launch.sh` is a
launch-option wrapper. Set appid 485510 to `.../tools/quiet-launch.sh
%command%`, and the script reproduces the invocation this machine already uses
(`ENABLE_GAMESCOPE_WSI=0 gamescope -f -w 1920 -h 1080 -W 1920 -H 1080 -r 120 --`)
for normal play. It switches to headless gamescope under a capped scope when
`~/.cache/nioh3p/headless.flag` exists (`--headless-on` / `--headless-off` /
`--status` / `--dry-run`). Launch options are per-title, hence the flag file
rather than a second launch-option line.

A dump without Steam is also possible. The DRM stub only unpacks when Steam is
in the chain, and the reason is precise. The copy's own log says
`[S_API] SteamAPI_Init(): Sys_LoadModule failed to load:
C:\Program Files (x86)\Steam\steamclient64.dll`. Copying the Steam client into
the throwaway prefix (below) let the process live, and it did produce
50,782,208-byte dumps headlessly. `SteamAPI_Init` still fails against that copy,
so the run never authenticates and the dump's `.text` stays mostly encrypted:

```sh
SC="$HOME/.cache/nioh3p/smoke/pfx/pfx/drive_c/Program Files (x86)/Steam"
mkdir -p "$SC" && cp ~/.local/share/Steam/steamclient64.dll "$SC/"
tools/quiet.sh game
```

Measured on such a run: the boot snapshot holds 436 of 4,505 `.text` pages
changed relative to the encrypted on-disk image. The settled snapshot, taken 20 s
later, holds 4,505 of 4,505. Every page has entropy 5-6, which means real code.
The unpacking is therefore a bulk pass during the first seconds of the run, and
a snapshot taken too early misses it. `dump_every_s` provides periodic
snapshots, so a run's coverage is never a guess. Both dumps are 50,782,208 bytes
and live in `~/.cache/nioh3p/dumps/`. A complete decrypted image of the game is
available without Steam and without a window, and everything in 6.5 below was
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

Everything below comes from `nioh3p_image_settled.bin`. All addresses are VAs at
base `0x140000000`.

The six `EOS_Lobby_CreateLobby` call sites build their options on the stack.
`MaxLobbyMembers` comes from an object field. The code below is around the site
that the runtime log already reported (`nioh.exe+0x5d915d`, that is
`0x1405d915d`):

```
1405d90fa  mov 0x4(%rbx),%eax        ; src->+4
1405d90fd  mov %eax,0x30(%rsp)       ; options.MaxLobbyMembers (options base = rsp+0x20)
1405d9113  mov %rcx,0x28(%rsp)       ; options.LocalUserId
1405d9123  mov %ecx,0x34(%rsp)       ; options.PermissionLevel
1405d912a  mov %eax,0x38(%rsp)       ; options.bPresenceEnabled
1405d9101  lea 0x...,%rax / 1405d9108 mov %rax,0x40(%rsp)   ; options.BucketId
1405d915d  call *0xbc207d(%rip)      ; -> EOS_Lobby_CreateLobby (IAT 0x14119b1e0)
```

The field offsets match the EOS 1.x layout exactly: `ApiVersion +0`,
`LocalUserId +8`, `MaxLobbyMembers +0x10`, `PermissionLevel +0x14`,
`bPresenceEnabled +0x18`, `bAllowInvites +0x1c` and `BucketId +0x20`. The bucket
string is `"Nioh"` (`0x1415864EC`). The requested service limit comes from an
object's `+4` field, not from an immediate at each call site. One API hook
covers all six service calls. It does not establish that game-side arrays or
mission state use that enlarged service limit.

The six sites sit in six wrappers of about 0x180 bytes. One wrapper is a
`std::_Func_impl_no_alloc<lambda_...>`, and these are the lambda bodies that 6.3
described:

| site | wrapper function |
|---|---|
| `0x1405d3dbd` | `0x1405d3cc0` |
| `0x1405d75be` | `0x1405d74c0` (`std::_Func_impl_no_alloc<lambda_44d22d62...>`) |
| `0x1405d85dd` | `0x1405d84e0` |
| `0x1405d915d` | `0x1405d9060` |
| `0x1405d9bfd` | `0x1405d9b00` |
| `0x1405de0a0` | `0x1405ddfb0` |

`CSessionP2pOpenCreateParam::SetParam` is vtable slot 2. Its body at
`0x1405F0FF0` copies the requested member limit and deep-copies metadata
attributes separately. The earlier interpretation of its descriptor loop as
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

This code establishes an attribute descriptor table. It does not establish a
structurally three-slot member table. Native member-array sizing is in the
create worker at `0x1406049DD`. It reads the request's `+0x20` and allocates the
primary array. It allocates an additional `limit-2` array when the limit
exceeds 2. Those allocations happen before
`ISteamMatchmaking::CreateLobby`, so a service-only clamp is too late to resize
the host.

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

No one has published this work for Nioh 1. There is no player-count mod and no
netcode reverse engineering, not even a plugin loader for the first game. The
sequels have only the loading mechanism, a `dinput8.dll` proxy, which is the
same approach used here. They also show the cross-game pattern. A bigger lobby
needs the host alone, while the gameplay side must match on every client.

We identified Nioh 1's containers ourselves, because no public spec exists.
`archive_XX.lnk` starts with magic `K300`, followed by a count and per-archive
records. Each archive is described by `lfm_order_XX.bin` (magic `LFMO`, version
1), which lists the original build paths, for example
`E:\workspace\shared\Nioh\application\data\DX11_data\archive\archive_06.cpp`.
It also holds a SHA-256-style hash per entry. Payloads open with their own
markers (`3SPK1000`, timestamps), so they are compressed streams. A full
extractor is its own project, and it is not on the critical path for
multiplayer.

## 8. What a working third player needed, and what is still open

A bigger lobby alone did not give a playable third participant. These are the
game-side checks and their status after the 2026-10-20 runs:

1. Session and member bookkeeping: fixed. The host allocates three native
   members from its request. The visitor allowance admits a second visitor
   ("Host visitor allowance").
2. Mission sync: joins, independent movement and a replicated attack were
   observed with three participants. Death and revival, boss fights and mission
   completion were not exercised.
3. Player slots: all three spawn and render on every client, with correct
   per-client slot tables. Targeting, loot ownership and rewards are unverified.
4. UI and menus: the party HUD names only one partner. Otherwise admission and
   play work through the normal menus.
5. Save and profile: three-player mission records have not been validated.

The fix came from tracing the actual rejection, not from patching constants that
equal two. Keep that method for the remaining items.

## 9. Realistic paths forward

1. Ship the third player through the lobby, then patch the session layer
   incrementally. This is what the repo is set up for. Use the dump and the log
   to find the 2-player assumptions. Patch them in memory from the proxy and
   test with 3 clients. This path has the highest fidelity and the highest
   effort. The next concrete step is to hook the `CSessionP2pMember`-related
   allocation and the `CPacketModifySessionInfo` path in the dumped image.
   Start from the six `EOS_Lobby_CreateLobby` wrapper call sites and the
   session object whose `+4` field feeds `MaxLobbyMembers`.
2. Determine how far the lobby clamp alone gets, before touching code. Run the
   game with the mod and create "Yokai Realm with a Companion". See whether a
   third client can even be invited and accepted. If the game already tolerates
   a third member at the lobby level, the patch surface shrinks a lot. This
   needs three accounts, and it is the cheapest experiment worth doing.
3. Data-level option: the `K300` containers are readable now (section 6.2,
   `tools/lnk.py`). They hold 13,382 entries over 20 archives, and the script
   archives carry the mission logic and its 1,072-name engine API. If the
   player count is a table or script value anywhere, that is far safer than
   code patching. The containers have no name table, so the next step is to map
   the loader's `%s%s_%02d.lnk` prefixes (encrypted section) or to identify
   entries by content.
4. Buy the sequel instead. Nioh 2: Complete Edition (Steam 1325200) and Nioh 3
   both support three-player co-op natively. Nothing in the Nioh 1 codebase was
   designed for it. Neither game is in this Steam library today.

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

Static facts in sections 1-3 come from `tools/pe.py`, which reads the PE,
section, import and export tables. They also come from `tools/analyze.py`, which
does the `.pdata`-bounded disassembly. Section 5's results are the actual log
output of the two smoke tests.

## 11. Call-site naming, the online layer's class map, and the discovery gate

Everything in this section was established offline, with no game session.

### 11.1 The proxy now names what it sees

`tools/sites.py` turns the RTTI class map into `nioh3p.sites.txt`. The file
holds 11,414 method addresses and 2,324 vtables, class-qualified. The proxy
loads it from its own directory and uses it two ways:

* call sites: nearest entry at or below the caller's RVA, logged as
  `site: JoinLobby from CSessionP2pOpen::vfn2+0x3a`
* callback objects: exact vtable match, logged as
  `steam cb: RegisterCallback id=333 ... class CCallback<CSessionP2pSteam, GameLobbyJoinRequested_t>`

The file is optional. A missing file gives `exe+0x...`. `install.sh` installs
the file, and it replaces `cp` with write-and-rename, so a reinstall cannot
corrupt a running game. The harness verified the sidecar load, the nearest-below
resolution down to `+0xc8`, and the class resolution for a second interface
type.

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

These Steam callbacks were registered in a live session. Id 333 is
`CCallback<CSessionP2pSteam, GameLobbyJoinRequested_t>`, id 505 is
`CCallback<CSessionP2pSteam, LobbyDataUpdate_t>`, id 507 is
`CCallback<CSessionP2pPacketHandler, LobbyChatMsg_t>` and id 304 is
`CCallback<SteamFriend::CSteamFriend, PersonaStateChange_t>`. Id 331 is
`GameOverlayActivated_t`, and 1101/1102/1103 are stats. No rich-presence-join
handler exists. Invites arrive only as `GameLobbyJoinRequested_t`.

### 11.3 Why a hosted session is invisible to the client's search

`docs/RE_KTOL_KEYS.md` holds an offline xref analysis of the runtime image. It
found the search filter:

```c
/* CSessionSearchHandle helper 0x609EA0, call at 0x14060A14E */
AddRequestLobbyListStringFilter("ktol_lock_status", "ktol_session_unlock", 0);
```

The EOS path does the same with `EOS_LobbySearch_SetParameter`
(`ktol_lobby_lock_status` == `ktol_lobby_unlock`, call at 0x1405DBC27). These are
the consequences:

* A locked session never appears in the search results. The client cannot see
  it, so it never gets to join. This is the observed symptom: "he started
  hosting and my search does not detect it".
* Neither `EOS_Lobby_JoinLobby` nor `CPacketJoinRequest` tests the lock, and no
  `GetLobbyData` of the lock keys exists anywhere. The lock gates discovery, not
  the join.
* Lock state is one global byte `0x1417F9840`. The setter is `0x5EAF60`, and the
  emitters are `CSessionP2pLockHandle` `0x5FB560`/`0x5FB810`.

### 11.4 New option: `drop_lock_filter`

`nioh3p.ini` with `drop_lock_filter=1` makes the proxy swallow the lock filter
in the search. `AddRequestLobbyListStringFilter` with a `lock_status` key is
logged as `DROPPED` and not forwarded. The EOS `EOS_LobbySearch_SetParameter`
with a `lock_status` parameter returns success without setting it. Clients then
see locked sessions. The default is `0`, which is stock behavior.

The harness verified it. With the option off, the filter reaches the fake
service (1 call). With the option on, it makes 0 calls, and the log shows
`Steam lobby filter: "ktol_lock_status" = "ktol_session_unlock" DROPPED (drop_lock_filter=1)`.

The shipped default changed to 0 on 2026-10-21. The player package must not
disable the game's lobby filters. `nioh3p.ini` and the compiled-in defaults now
ship `drop_lock_filter=0 drop_slots_filter=0`, so a stock install keeps the
game's normal search rules. A locked or full session then stays hidden. The
earlier three-player run was verified with both drops at `1`. That combination
is now the documented opt-in workaround, not the shipped configuration. The
headless rig's generated ini (`tools/rig/setup.sh`) still sets both to `1`. A
three-player discovery run with the shipped defaults (both `0`) is still
outstanding. That run is what makes the new default verified end to end.
`force_unlock=1` remains the host-side alternative.

### 11.5 What else the analysis settled

* Join-time validation exists at `0x5F2010`. It builds `integer_<n>` keys,
  reads them with `GetLobbyData`, decodes them and `memcmp`s them against the
  expected record. It then compares the lobby's `ktol_owner_id` against
  `GetLobbyOwner`. The mod already watches both calls (slots 19 and 35), so the
  next session's log shows this handshake and where it stops.
* Client capacity is service-driven, and host capacity is request-driven.
  `0x603C80` sizes client arrays from `GetLobbyMemberLimit`, with an extra
  `limit-2` array. The create worker instead reads `request+0x20` at
  `0x6049DD`, before `CreateLobby`. Live reads showed native capacity 2
  and no extra array with only the service clamp, then capacity 3 and an
  allocated extra array after the request hook.
* A gap remains for EOS hooks. `EOS_LobbyModification_SetMaxMembers` and
  `EOS_LobbySearch_SetParameter` resolve from the EOS SDK, but they are "not
  usable in the import table (present=1)" and the writable-memory pointer scan
  finds nothing. Those two hooks are therefore written but not armed.
  `docs/RE_EOS_CALLSITES.md` covers the exact slot to patch.

### 11.6 Instruments added this session

These instruments were added this session. Lobby-list search covers
`RequestLobbyList`, every `AddRequestLobbyList*` filter and `GetLobbyByIndex`.
Friends cover `GetFriendCount`, `ByIndex`, `GamePlayed`, persona state and rich
presence keys. Legacy P2P covers `SendP2PPacket`, `IsP2PPacketAvailable`,
`ReadP2PPacket`, `Accept`/`CloseP2PSessionWithUser` and `GetP2PSessionState`.
Sockets cover `ConnectP2P`, `CreateListenSocketP2P`, `SendMessageToConnection`
and `CloseConnection`. The set also covers invites, per-member data and callback
payload logging, plus the call-site and class naming of 11.1. Each instrument
ran in the harness before install.

## 12. Static session-open and invite-path analysis

The only entry into a Steam session open is `SessionOpen_Start` (`0x5E9440`). It
requires `[CSessionP2pSteam+8] & 2`, no error bits, and
`manager->vfn4() == NULL`. It then calls `0x5F71F0`. That function writes the
request's mode dword (`0x5F72B4` / `0x5F7328`) from the descriptor's kind at
`0x5F7274`. Value 0 means create, and 1 means join. The function also attaches
the static `CSessionP2pOpen` object (`0x1417F96C0`) through
`CSessionP2pHandle::vfn3` (`0x5EFB50`).

`CSessionP2pOpenHandle::vfn2` (`0x5F6B20`) branches on that mode:

| mode | path |
|---|---|
| 0 (create) | embedded `CSessionCreate` (+0x20) → `CreateLobby` at `0x604CFA` |
| 1 (join) | embedded `CSessionJoin` (+0x48) → `CSessionJoinHandle::vfn2` state 1 → **`JoinLobby` at `0x6038F2`** |

Those two sites (`0x6038F2`, plus the caller-less `0x603F03`) are the *only*
`JoinLobby` callers in the image, and both require mode 1.

The inspected callback 333 handler (`0x5EF7E0`) stores the lobby id at `+0x260`
and calls `RequestLobbyData` (`0x5EF80E`). The mapped `LobbyDataUpdate` handler
(`0x5EF820`) sets `+0x254` to 2 or 3. The inspected pump path at `0x5EF03B`
produces error `0x80C81010`. This is not proof that stock invites can never join.
The earlier live traces used broken callback and SteamID-return hooks (§14). A
clean baseline is required before attributing “Connecting...” to game logic.

The mapped `ktol_invite_data` references (`0x5F0418` read, `0x6051A3` write) do
not establish a capacity gate. Two corrections apply to the 11.2 backtrace
reading. `0x604CE3` is inside the create worker (`0x604940`–`0x604F14`, its
`SteamInternal_ContextInit` call). `0x5EF800` is inside the invite handler
(`0x5EF7E0`). That frame was a stack-walk artifact, not a poll chain.

### 12.1 Located lobby filters and capacity inputs

| gate | where | option |
|---|---|---|
| locked sessions hidden from the search | `AddRequestLobbyListStringFilter("ktol_lock_status", "ktol_session_unlock")` at `0x60A151`; EOS twin `EOS_LobbySearch_SetParameter` at `0x5DBC27` | `drop_lock_filter=1` (client) or `force_unlock=1` (host) |
| full sessions hidden from the search | `AddRequestLobbyListFilterSlotsAvailable` at `0x60A158` | `drop_slots_filter=1` (client) |
| host lobby capacity | `CreateLobby` `cMaxMembers` (Steam vtable slot 13) / EOS `CreateLobby` options+0x10 | `max_players=3` |

The inspected search route reaches `SessionOpen_Start`, then mode 1, then
`JoinLobby`. Raising capacity and removing these filters does not establish that
a third member can complete that route. It does not establish participation in
mission gameplay either.

### 12.2 Located EOS capacity calls

`EOS_LobbyModification_SetMaxMembers` has no IAT slot and zero references in the
image. The game never calls it, so the "not hooked" line was not a gap. The only
EOS capacity input is `EOS_Lobby_CreateLobby`'s `MaxLobbyMembers`, at all six
call sites and slot `0x14119b1e0`. That input is already clamped.

Two structural fixes came out of it. `install_hook()` now falls back to finding
an import slot by value. The writable-only pointer scan never saw an IAT
slot, because IAT pages are read-only. `build.sh --harness` also regenerates the
import libraries every run. A stale `libeossdk.a` silently hid new `.def`
entries.

## 13. EOS ABI and ownership corrections (2026-10-18)

The previous proxy itself had defects that corrupt the join path:

* `EOS_LobbyDetails_CopyInfo` released the SDK result before returning it to the
  game. The detour now leaves ownership with the caller.
* `EOS_LobbyDetails_GetMemberCount` returns the count directly. It has no output
  pointer. The old detour interpreted an unspecified third argument as a pointer
  on the zero-count path.
* `EOS_Lobby_AttributeData` stores its value union before `ValueType`.
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
* Protected-page CopyInfo lifetime regression: the old DLL exits 3 with
  `FAIL: CopyInfo result was released before its caller consumed it`. The
  corrected DLL exits 0. Reproduce with `./build.sh --harness` followed by
  `DISPLAY=:9 tools/smoketest.sh ownership`.

These are local ABI/lifetime checks, not a successful online join.

One more static check: `CPacketModifySessionInfo::vfn1` at `0x14060C1B0`
serializes a single byte from object offset `+0x14`, guarded by output-buffer
capacity. `CPacketJoinRequest::vfn1` at `0x14060CB50` serializes a byte, a field
through `0x1405FC530` and a length-prefixed string. Neither inspected serializer
proves a two-member array cap. Their packet consumers and mission behavior still
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

The corrected fixture uses those marshaling instructions. It does not pretend
that the flat and virtual signatures are identical. With the pre-fix DLL, its
async-result regression exits 4:
`FAIL: matching call result or I/O failure was lost`.
With the corrected DLL, `tools/smoketest.sh proxy` exits 0. A matching error
result completes once, and mismatched or duplicate results do not consume
another completion. The requested lobby gets limit 3, and full 64-bit
lobby/member/friend identities survive dispatch. `tools/smoketest.sh ownership`
still exits 0. These fixtures are not an online Steam service.

The actual game also reached its opening movie and interactive first-run setup
in the isolated rig. The previous `DXVK_WSI_DRIVER=xcb` setting caused
`Failed to initialize WSI`. Removing it exposed Xvfb's Vulkan surface failure.
Headless gamescope on the iGPU provides a working surface, while Steam's UI
stays on Xvfb. See `RIG.md` for the corrected launch path. A rendered game is not
a successful multiplayer join. Concurrent licensed access remains blocked.

The final build was installed into slot a and relaunched through the persistent
`quiet-launch.sh` Steam launch option. The real game rendered and delivered a
`GlobalStatsReceived_t` call result at 14:08:37. Its 64-bit handle and
`ioFailure=0` are visible in `logs-a/nioh3p.log`. This is a live callback smoke,
not a lobby result. The same verified DLL was then installed into the main Nioh
installation, and its existing INI settings were preserved.
`~/nioh3p-test/shots/a-title-ready.png` captures the corrected build at Nioh
1.24.08's “PRESS ANY BUTTON” title screen after first-run setup.

## Rig boot investigation: corrected evidence (2026-10-20)

Earlier notes blamed slot b's missing game process on a clean return from
`WinMain`, an emulator-client kick or a file-deleting agent. Those causes were
not established. Silent hooks on the game's `ExitProcess` and `TerminateProcess`
imports cannot observe every termination path. A surviving Steam UI is not
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
loading phase. It does not prove which game or stub instruction requests the
relaunch. It does not prove what every older run did either. The compositor
subsequently displayed Steam's Big Picture UI, captured in
`shots/resumed-b-baseline.png`. Inspection of live process environments placed
that Linux Steam client under `home-b/.local/share/Steam`, not the user's real
Steam root. `xalia.exe` was also in slot b's prefix. Its presence does not
establish a launch-broker role.

The installed Steam API DLLs were compared byte-for-byte. Slot b differed from
slot a only in six bytes at file offset `0xbd150`, from an earlier experimental
`SteamAPI_RestartAppIfNecessary` return-false patch. Slot b was restored from
slot a's unpatched emulator DLL. The shipping proxy no longer suppresses Steam's
restart decision. Replacing a Steam API implementation is not proof that this
protected game executable can boot without its normal launcher.

With the cloned Linux Steam client already running, full `proton run` launches
subsequently reached Nioh on both slots. Slot a loaded the level-26 Kyushu save.
The updated proxy was installed in both `lan-*` copies and exercised in the
actual game. An intermediate `runinprefix` experiment instead used WineD3D and
faulted at `nioh.exe+0x52ffe9`. It is not an equivalent graphics launch.

The original slot-b compositor also logged `Unhandled event EI_EVENT_SYNC (91)`
and `Unhandled libei event!` during injected input. The game animated its title
but ignored input, even when both X11 and Windows reported it as foreground.
SIGTERM had left several rig-owned compositors alive. Their launch logs show
socket collisions and a fallback to `gamescope-1`. Those remaining processes
were identified by rig HOME and force-stopped. The cloned Steam client was
restarted on Xvfb `:10`, then the game in a fresh headless compositor. Slot b
then accepted input and reached its main menu (`shots/b-clean-title.png`).

### Live matchmaking hook proof

On slot a, the path Torii Gate → Yokai Realm with a Companion → With Conditions
→ Mission → Kyushu → Wreathed in Flame → Search first queried lobbies. It
created one when none matched. This screen can therefore host, not merely
visit. The live trace records:

- `19:15:17.429`: `CreateLobby(type=2, maxMembers=2)` clamped to 3.
- `19:15:17.512`: successful `LobbyCreated_t` (`EResult=1`), lobby
  `0x018600001e4b44dd`.
- Published owner `Nioh-LAN-A`, SteamID `<steamid-lan-a>`, and unlocked
  session metadata.

The game stayed at “Searching for a companion…” while it owned that lobby.
This proves that the emulator-compatible hooks work in the game, and that the
creation request uses capacity 3. It does not prove that a third actor can join
or play.

### Verified two-player LAN gameplay

Slot b resumed Wreathed in Flame and held `E` at the starting shrine to pray,
then selected Summon Visitor and made an Ochoko offering. Slot a used Torii
Gate → Random Encounters → Become a Visitor → Without Conditions → Search.

- B created lobby `109775241935911929` at `19:25:41.813`, requesting 2 members.
  The proxy forwarded 3.
- A's `GetLobbyMemberLimit` returned 3 at `19:25:46.668`, then it called
  `JoinLobby` at `19:25:46.685` and received its `LobbyEnter_t` result.
- Both clients rendered the two actual characters in the same mission, with
  the remote name and health bar visible: `shots/b-two-player-host.png` and
  `shots/a-two-player-visitor.png`.
- Moving A backward changed A's position in B's view, captured in
  `shots/b-visitor-movement-observed.png`. P2P packets were exchanged between
  distinct identities `<steamid-lan-a>` (A) and `<steamid-lan-b>` (B).

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
  Steam API boundary. The ownership cause was identified below.

That zero-result search was not evidence of Steam reporting a full lobby.
It did not reach third-player admission. The later native-admission proof below
is a separate result.

### Native host allocation and metadata ownership fixes

The API clamp ran too late for the host's own arrays. A live host advertised
three Steam slots, while `CSessionP2pSteam+0x198` remained 2 and its extra-member
array at `+0x1a8` was null. The create worker reads request `+0x20` at
`0x1406049DD` and allocates the arrays before it calls Steam at `0x140604CFA`.
Hooking `CSessionP2pOpenCreateParam::SetParam` before that worker produced
native capacity 3 and a non-null extra-member array. A still joined gameplay.

The metadata failure was descriptor self-assignment, not a compatibility filter
to remove. A bounded debugger capture showed that the input descriptor and the
destination descriptor were the same object. The publisher freed its old binary
buffer at `0x1405FFEDC` and stored the replacement pointer into that same
descriptor. It then copied from the now-replaced source pointer at
`0x1405FFF5E`, which effectively copied the uninitialized replacement to
itself. The proxy's guarded continuation at `0x1405FFE90` skips only that
redundant local replacement for exact descriptor self-assignment. It retains
network and Steam publication and the original non-alias path. The live host
installed the guard at `20:46:06.174`. A late C search at `20:47:44.785` read
the original canonical `binary_84` value and reached `JoinLobby`.

### Three native members accepted

Bounded Win32 debugger probes on C then established more than a lobby callback:

- C's native array capacity was 3 before response execution.
- `CPacketJoinResponse::Execute` at `0x14060B090` received status 1, local index
  2, owner index 0, ten metadata attributes and three member records.
- The records at response `+0x438`, stride `0xa8`, held distinct B/A/C IDs
  `<steamid-lan-b>`, `<steamid-lan-a>`, `<steamid-lan-c>` and their correct
  `Nioh-LAN-B/A/C` names. Response `+0x2c` counts metadata descriptors, and
  `+0x430` counts members. `+0x4cc` is a field within the first member, not
  the start of the member array.
- At `0x14060B38C`, the executor wrote latch value 1 to
  `[manager+0x190]->+0x18->+0x4e4`. The native join handle maps 1 to success.
- A separate normal-close operation followed with the latch still 1. Its
  public native path was captured through `0x1405F4EE0`, with game-side
  close wrapper return `0x140D71DCC` and higher caller `0x140D64246`.

This was native three-member admission, not yet mission gameplay. The next
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

The packet dispatcher uses two-stage mapping, not an ordinal switch. `rawID - 1`
indexes byte table `0x140D6C2E8`, which indexes target table `0x140D6C228`. Raw
ID 7 reaches `0x140D6AE0A`. Payload byte 2 reaches `0x140D6AF3F`. Earlier
case-ordinal labels must not be treated as raw IDs. The host probe for the
incorrectly labeled ID-5 handler timed out, restored its breakpoint and
detached. That negative result says nothing about raw ID 7.

The visitor's join request is raw ID 16, sent at `0x140D7A68F` with
`ecx = 4 + 0xc`. The host handles it at `0x140D6B3DA`. `0x140D6B327` is the
tail of the raw-12 handler, not the admission handler.
`0x140D6B4B2–0x140D6B4C7` sends raw ID 7, reason byte 2 and four payload bytes
to the incoming sender:

- Operation `+0x3c9` must be zero. The sender slot's `+0x104` flags must lack
  masks `0x8` and `0x4`.
- Request byte 2 is stored into sender slot `+0x10b`. A nonzero value selects
  `root[+0x39d] - D7A1E0(root)`, and zero selects
  `root[+0x39c] - D79E50(root)`.
- Both counters scan non-host slots 1–3, requiring slot `+0xe8 == 2` and flag
  `0x4`. They partition slots by nonzero versus zero `+0x10b`.
- When DWORD `0x14191DE34 != 2`, signed delta `<= 0` selects reason 2.
  For value 2, the alternate `D8673F0` check selects that response instead.

### Host visitor allowance: the rejecting branch, and the fix

A read-only 10 ms poller on B (no debugger) captured the decision inputs:

| moment | `root+0x39c` | `0x14191DE34` | `0x14191DE38` | slot 1 (A) | request byte |
|---|---|---|---|---|---|
| boot (root reset `0x140D7AC0C`) | 1 | 0 | 0 | – | – |
| shrine offering (`0x140D7BB77`) | 1 | 0 | 0 | – | – |
| A admitted | **0** | 0 | 0 | `e8=2 f=04 t=00` | 0 |

`root+0x39c` is the number of ordinary visitors the host can still admit. A's
request computed `1 - 0 = 1`. Because `0x14191DE38 == 0`, the accept path then
cleared the consumed allowance (`0x140D6B539`). C's request computed
`0 - 1 = -1` and took the raw-7 reason-2 reject. Writing `root+0x39c = 2` into the
live host (diagnostic only) let C's next request through (`e8=2 f=04`), and C
entered the mission.

The game already contains a two-visitor value. Its own shrine setter
`0x140D5FB30` accepts values below 3, and an item path (`0x140CFA480`, item
`0x74CC`) raises 1 to 2. The proxy now changes the single-visitor constant at
every store that opens recruitment. Those stores are root reset `0x140D7AC0C`,
shrine offering `0x140D7BB77`, controller reset `0x140D7CA03`, start-recruiting
`0x140D7DAA8` and the online shrine setter argument `0x140CF9F36`. The new
value is `max_players - 1`, capped at 2 by the game's own `< 3` check on the
setter argument. Consumption, departure (`0x140D7B234`) and close paths are
untouched. Installation waits for the exact original instruction bytes after
`.text` decryption and logs
`game visitor allowance: recruitment opens 2 visitor slot(s)`.

### Three players in one mission (2026-10-20)

With the allowance build on all three slots, cold-launched and with no memory
writes, B offered at the Wreathed in Flame starting shrine (allowance 2). A
searched and was admitted, and the allowance stayed 2, because `2 - 0 = 2` is
not the last slot. C then searched and was admitted, and `2 - 1 = 1` consumed
the allowance to 0. All three rendered each other in the mission, and each
moved independently. The other two clients showed the movement in
`shots/f-{b,a,c}-3p-1.png` and `shots/f-{b,a,c}-3p-move.png`. Each client's
slot table converged to three connected members (`e8=2`, flags `0x79`/`0x7d`).

A second clean run (all three slots cold-launched with the final
`154a5a1d…` build) repeated this with C joining first and A second, and added:

- C's light attack consumed C's Ki (104 → 93) and B rendered C mid-thrust
  while A stood apart: `shots/z-c-attack.png`, `shots/z-b-sees-c-attack.png`.
- Host slots for both visitors kept the admission flag (`f=7d` each).
- C then quit normally. B and A stayed in co-op (B's party HUD switched to
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
write to the game install, the save folder or the system. The only files are
`nioh3p.log`/`nioh3p.prev.log`, in the game folder with a `%TEMP%` fallback.
Every code write was byte-checked or identity-checked. Fixes shipped from it:

- `nioh_launcher.exe` is x64 and also imports `DINPUT8.dll` (`objdump -p`), so
  the proxy ran there too, rotating the game log away. The proxy now acts only
  in `nioh.exe` (and the harness host `testgame.exe`). Elsewhere it only
  forwards DirectInput.
- On any other `nioh.exe` build all EOS, Steam and game hooks are skipped
  (`unsupported nioh.exe build … mod inactive`). Before, the EOS/Steam hooks
  installed regardless, and their detours read hard-coded struct offsets.
- The metadata-guard patch is one `lock cmpxchg16b` of the 16-byte-aligned site
  instead of four compiler-generated stores, so no other thread can execute a
  torn instruction. The extra RAX/R10 writes in the guard were checked dead
  against the disassembly. The `0x5ffea2` paths write RAX first, and R10 is
  unread before the next call.
- A Steam vtable slot is patched only if its current value points to
  executable memory. `max_players` outside 0/2–4 becomes 3, and
  `DllCanUnloadNow` always answers `S_FALSE`.

Verification passed. The harness passed at trace=0/1 plus the ownership test.
The harness renamed `nioh_launcher.exe` and wrote no log. Renamed `nioh.exe`
(wrong identity) logged the inactive line, and its clamp test failed as
intended. `max_players=9` logged `out of range; using 3`. The rig repeated host
B + A + C in one mission with this build, and the host logged the guard install
at C's join. `nioh.exe` has no CFG (`DllCharacteristics 0x8160`) and imports
only `DirectInput8Create` from dinput8. One point is not testable here: Defender
or Smart App Control behavior on an unsigned DLL. The player README covers the
recovery, which is to delete `dinput8.dll`.

The lean build (`502c5508…`) removed dead code and one-caller wrappers, and
moved the visitor-allowance poll after the image dump. It repeated the run. B
offered, A joined in 38 s, and C joined in 39 s with A paused in bursts. The
host's guard installed at C's join (19:03:29), and B rendered both visitors
(`shots/lean-b-3p.png`). Logs grew 0 bytes over a minute of play. The first C
launch crashed in the intro movie, before any lobby activity. The crash was a
null COM pointer inside `nioh.exe` (`0x14052f321`, DirectShow playback), with
no proxy frame. The relaunch was fine. At quit, two gamescope wrappers exited
134 on a libei teardown assertion after the game had shut down.

#### Secret Word, and hardening for players (2026-10-21)

The matchmaking search filters were recorded on the real game, with trace=1 and
one visitor searching with Secret Word Off. The values were `integer_80`=69,
`integer_76` ≥1, `integer_77` ≤4, `integer_78` ≤1, the `binary_84` blob
`MDE2NgEBAQEBATIwAA==`, `ktol_lock_status`=ktol_session_unlock, distance 3 and
max results 20. The game repeats the search with `integer_81`=4 and
`integer_83`=0 variants. No filter key contains "secret" or "password", and the
same keys appear in a 12-hour trace. The Secret Word is therefore not a separate
key.

The UI explains why. On *Become a Visitor* the Secret Word row is a preset
selector (`< Off >`) that stays on Off until conditions are set
(`shots/sw-bv4.png`, `shots/sw-sw5.png`). The Summon Visitor offering screen has
no word field at all (`shots/safe-b-offer1.png`). The mod's tested flow therefore
needs the word Off. A word can only match a host that advertises the same word,
that is, on the Torii Gate *Yokai Realm with a Companion* path.

The mod can change exactly two filters. One is the `lock_status` string filter,
and the other is the slots-available filter. Each has its own ini key, and both
are off (`0`) in the shipped configuration, so the default build forwards both.
Every other call is forwarded byte-identically. Player mode now logs each
distinct filter key once (`lobby filter: pass-through "<key>"`, key only, never
the value). A player log from a Secret Word session therefore shows the
word-bearing filter. The harness proves it. Two unknown probe keys
(`nioh3p_probe_key`, `nioh3p_probe_num`) must appear as pass-through lines in
the mod's own log.

Stock filters are sufficient. This result is from 2026-10-21, build
`1003e4e7…`. With `drop_lock_filter=0 drop_slots_filter=0`, the shipped
defaults from this build, host B raised the lobby to 3. Both visitors found and
joined it (JoinLobby 22:44 and 22:47, both visitors visible on the host,
`shots/stock-3p.png`). The visitor logs contain zero `DROPPED` lines, and the
host installed the metadata guard at the second join. The earlier filter drops
were therefore a workaround from before the create-request clamp, not a
requirement. They stay as opt-in diagnostics for a host that cannot be found.

Hardening in the same build covers five points. EOS option structs are gated on
their `ApiVersion` (game: 7 for `EOS_Lobby_CreateLobby`), and a mismatch
forwards them unmodified. Unresolved EOS hooks are retried. Configuration and
log files are opened from the worker thread instead of `DllMain`, so nothing
runs under the loader lock. A `%TEMP%` that resolves to the Windows directory is
refused. Every launch ends with one `preflight:` line, which states whether the
exe is supported, how many hooks are installed and where the log is.

#### Rig artifact: second-visitor load stall

On this rig the visitor that joins second can sit on the loading screen for
~280–300 s. This reproduced with C second and with A second. The first visitor
loads in ~20 s. Network admission is immediate (state 9), but the mission loader
stays in phase 1 (`*(0x1418715E0)+0x5D0`). A diagnostic build, since removed,
held a temporary script-binding call counter. It showed the mission script
polling only `Load::IsLoadEnd` (`0x14089BF90`, resource-manager completion) at
1–5 calls per 5 s, about one frame per second. The integrated GPU (`1002:13c0`)
reported 100 % busy with the two in-mission clients. Pausing the first visitor's
process (`SIGSTOP`) for 8 s let the newcomer render. `Load::IsLoadEnd` jumped to
131 calls per 5 s, and its load completed inside that window. In the clean run,
pausing C in 5 s bursts during A's join cut A's load to 33 s.

The stall is therefore GPU starvation from three game instances sharing one
iGPU, not a protocol wait. Its knock-on effects are also rig-only. The host
times the starved newcomer's slot out after ~90 s, and reconnection clears that
slot's admission flag `0x04`. When the first visitor later leaves, the host
counts no admitted ordinary visitor and ends co-op, with "Co-Op Defeat" for the
remaining visitor. With the flag intact (clean run above), the first visitor's
departure leaves the other two in co-op. Three candidates were ruled out along
the way. The member-slot connect gate at `0x140D794A0` was widened to states
9–12, which connected slot 1 early but did not shorten the stall. The coop wait
counters `N+0x15837C/0x158388` were zero on all three clients. The 18000-tick
timer at `R+0x20` was frozen during the stall.

Other observations:

- The party HUD lists one partner name. The third player renders without a name
  plate on some clients.
- While a visitor is present the host has no Pray prompt at the shrine. After a
  departure the allowance stayed 0 (departure only decrements it), so a
  replacement visitor cannot be admitted until co-op ends. The rig's rejoin
  tests reopened it with a diagnostic memory write.
- Attaching a Win32 debugger (`DebugActiveProcess`) to an in-mission client made
  it exit silently (A, exit 0, no proxy exit hook). Use read-only
  `ReadProcessMemory` pollers and brief `SuspendThread` sampling instead.
- C's earlier `XI_BadDevice` startup exits did not recur: C cold-booted three
  times this session (once with the previously failing 61,952-byte build, twice
  with allowance builds).

### Torii Gate companion hosting (2026-10-25)

This run tested the Torii Gate path that does not use an Ochoko Cup:
Yokai Realm with a Companion, Without Conditions, Search. The research build
of the `friend-invite` branch ran on B (`max_players=4`) and A, and the player
build ran on C. The Luna decision model drove every menu
(`tools/rig/coop.py`, traces `shots/coop-20261025-*.jsonl`).

The host side works. At boot B logged all seven allowance sites, including
`start recruiting (alt)` and `torii offering setter`, with 3 visitor slots and
the setter cap raised to 4. When B searched and found no lobby, the create
request was raised from 2 to 4 members before allocation. Steam then created
a lobby with `maxMembers=4`. The Ochoko counter stayed at `6/3/14`.

Two players work. A searched and joined the lobby of B. Both games showed the
preparation menu, chose a mission, counted down and loaded it, and each showed
the other player (`shots/torii-2p-b.png`, `shots/torii-2p-a.png`).

A third player does not get into the mission. The companion mode pairs the
host with one companion:

- C joined the same Steam lobby (`Steam JoinLobby` with stock filters, no
  dropped filter). C then showed `Waiting for mission to be chosen` with
  `Companion: Nioh-LAN-B`. The host menu shows one `Companion` field only.
- When A and C searched at the same time, both joined during the preparation
  countdown. The host chose the mission with A only. A loaded into the
  mission, and C stayed on `Waiting for mission to be chosen` until the run
  stopped.
- A client that joins after the mission starts gets the same waiting screen.
  The lobby stays listed while the mission runs.

The lobby and allowance patches are therefore not enough for this mode. The
next section finds the single-companion code and fixes it.

`CreateLobby` returns a Steam API call handle, not a lobby id. The proxy now
logs `Steam CreateLobby -> call N`. It logs the real id once per lobby, as
`Steam host lobby id=N`, when the host first publishes lobby data.

### Torii Gate with three and four players (2026-10-25)

A research-build packet trace (`game pkt send`/`recv` at the game's own send
and dispatch functions, below the encrypted Steam P2P layer) showed the cause.
The full packet map is in `docs/RE_TOKOYO_COOP.md`.

- Both later searchers were admitted (packet 73). The host then sent the
  mission preview (76), its ready flag (74), the chosen mission (17) and the
  start flag (75) to member slot 1 only. Each sender hard-codes "the other
  player" as slot 1. The companion in slot 2 never left "Waiting for mission
  to be chosen".
- With those four packets mirrored to slot 2 (a research hook), both
  companions received the mission, loaded it, and ran the load handshake with
  each other (18 to 21 already loop over all slots). All three clients then
  stayed on the loading screen with an idle iGPU. Read-only probes showed
  loader phase 1 on one companion, and a thread sample showed the mission
  script polling `Online::IsWaitCoopPlayer`. The sync counters read host 1,
  visitors 3: every companion increments the one shared visitor counter
  (mission flag `0x47`), so it runs ahead of the host.
- With the second fix (only the slot-1 companion advances flag `0x47`), the
  counters stayed equal (3/3) and all clients reached the mission.

Results on the rig, all menus driven by Luna:

| Run | Build | Result |
| --- | --- | --- |
| B host, A and C search together | research | all three in the mission (loader 12, state 14, counters 3/3), each client shows the other two (`shots/torii-3p-{b,a,c}.png`). A moved and B rendered the move. |
| the same | player `1750668e` | all three in the mission, 267 s from the coop start |
| B host, A, C and D search together | player `1750668e` | all four in the mission (`shots/torii-4p-{b,a,c,d}.png`), slots 0 to 3, counters 2/2 |
| C uses a Himorogi Fragment in the four-player mission | player | all four get "Yokai Realm with a Companion: Failed" and return to the Torii Gate. This is the stock rule for two players as well. |

The player build installs both hooks at boot (`game tokoyo: 2/2 companion
hook(s) installed`). On the host it logs `game tokoyo: packet N also sent to
companion slot S`. On a later companion it logs `companion slot S leaves the
shared wait counter to slot 1`. The party HUD and the result screen still name
one companion. Mission completion, death and revival with the assist gauge,
and rewards were not played through. They need a real session.

One rig artifact was found on the way. In research builds, `dump_every_s`
used to run on the worker thread before the allowance installer. A host with
`dump_every_s=180` patched no allowance site for 36 minutes, and the third
client left the Steam lobby 0.6 s after joining it. The dumps now run on their
own thread.

Windows 11 VM, Defender at its defaults (definitions 1.461.144.0, not updated
for this run): the player build `1750668e` passed the harness (6 PASS) with 0
detections, and the real `nioh.exe` loaded it (`preflight: exe=ok`) before
exiting with code 53 for lack of Steam. The companion hooks install only
after `.text` is decrypted, so they did not run in the VM.

### Integrated live-test candidate (2026-10-25)

Both development branches are merged into `master`. The separate candidate
is `build/live-test/nioh3p-live-test.zip`. The player DLL is 44,544 bytes with
SHA-256 `632684d0a17e457abfdad2c18cf827e312f511ed74a0a08ee3a9a7238c188a81`.
The frozen release zip and the user's Steam installation are unchanged.

The merged four-player harness still expected a Steam member limit of three.
It failed with exit 5 while the proxy correctly sent four. The expectation
now follows the configured count for both EOS and Steam capacity calls.
`tools/quiet.sh` also hid that failure behind its final output command.
It now returns the underlying exit code. An invalid smoke mode returned 2
through the wrapper.

The integrated smoke matrix passed at both `max_players=3` and `4`:

| Mode | Result per player count |
| --- | --- |
| Player proxy | exit 0, 6 PASS, no FAIL |
| Research proxy | exit 0, 4 PASS, two player-only log checks skipped |
| Player ownership | exit 0, 1 PASS, no FAIL |

Artifacts are under `~/.cache/nioh3p/integrated-{3,4}p-t{0,1}-{proxy,ownership}`.
Only the combinations in the table were run.

Luna drove a fresh three-player Torii Gate run with the integrated player DLL,
`max_players=3`, and stock search filters on every client. Both visitors reached
the mission in 242 seconds. Read-only probes showed loader 12, state 14,
slots 0 to 2 and counters 2/2 on every client. The screenshots show three
characters in the same cave. Evidence: `shots/integrated-torii-3p.png`,
`shots/coop-20261025-151440.jsonl` and `runs/integrated-torii-3p`.

The four-player run also passed with `max_players=4` and stock filters.
Every client reached loader 12 and state 14 with counters 3/3.
The member slots were B=0, D=1, A=2 and C=3. The first companion was D,
so this run also exercised the wait-counter fix with a different first joiner.
Every client logged both companion hooks and all seven allowance sites.
The host mirrored packet 17 to slots 2 and 3. Evidence:
`shots/integrated-torii-4p.png`, `shots/coop-20261025-152049.jsonl`
and `runs/integrated-torii-4p`.

The same four processes then left through the host's Himorogi Fragment and
formed another group without restarting. All four reached the next mission
with loader 12, state 14 and counters 3/3. The new slot order was
B=0, A=1, D=2 and C=3. Evidence: `shots/integrated-torii-reform.png`,
`shots/coop-20261025-152641.jsonl` and `runs/integrated-torii-reform`.
This proves repeat group formation after an aborted mission, not completion
or reward handling. The rig then shut down with no rig processes remaining.

In the Windows VM, this exact DLL passed the rebuilt harness with exit 0
and 6 PASS. Defender real-time and behavior protection were enabled.
A custom scan found no new detection. The threat count stayed at zero.
Definitions were `1.461.144.0`, not updated for this run. This was a harness
test, not Windows multiplayer gameplay. No public scan or upload was made.
The Windows console output is saved privately at
`~/nioh3p-test/runs/integrated-windows-smoke.out`.
The test directory was removed from the guest, and the VM shut down cleanly.

### Launch-command and filesystem limits

Several archived compound commands used `pgrep -f 'lan-b'` or
`pkill -f 'gamescope --backend'` before copying files and launching. Those
patterns also match a `bash -c` command line containing the command itself.
They can kill the issuing shell before the copy or output-file creation runs.
A read-only demonstration reproduced this self-match. Such failures do not
establish that a different process later deleted or reverted the intended files.

The reviewed rig scripts contain no timed log deletion and no restart
supervisor. `tools/rig/setup.sh` explicitly trashes the stock-control slot's
proxy and INI under `home-b/.local/share/Steam/steamapps/common/Nioh`. It does
not install or manage the separate `lan-a` and `lan-b` emulator directories.
Other reported file disappearances remain unattributed, and no external cleaner
is known.

The rig prefix is inside its Steam root:
`home-b/.local/share/Steam/steamapps/compatdata/485510/pfx`. Trashing that root
also trashes its prefix and saves. Btrfs reflinks do not change this ancestry.
Future cleanup must target verified rig-owned PIDs. It must not target global
Wine or Steam names, or command-line fragments that can match the cleanup
shell.

Boot traces alone do not establish multiplayer. The two-player gameplay
evidence above is a separate check.

#### VirusTotal (player build `f239e4c1…`, 2026-10-21)

The build went to the VirusTotal web UI as an anonymous submission. Five of 71
engines flagged the DLL, all with generic heuristics: Avast and AVG
`Win64:Evo-gen [Trj]`, Avira `TR/W64.Evo`, WithSecure `Trojan.TR/W64.Evo` and
Cynet `Malicious (score 100)`. The popular threat label is "trojan". The other
66 engines reported clean, including Bitdefender, ESET, Kaspersky, Google,
Fortinet, CrowdStrike and SentinelOne's siblings. CAPA and the CAPE sandbox
reported 0 malicious behavior (one sandbox was still queued), so no dynamic
evidence exists. The flags are the usual family for an unsigned DLL that patches
another process's code. Permanent report:
`https://www.virustotal.com/gui/file/f239e4c1c8c227190ff2ef1978002c4e4571ab826f83164a57f7c40a1b053cd2`

For players, this has a consequence. Avast, AVG and WithSecure are common on
consumer PCs. A friend who uses one can see an alert, and Defender-class
quarantine stops `nioh.exe` from starting, because it imports `dinput8.dll`.
The README steps are the mitigations: restore the file, add a folder exclusion
and compare the SHA-256. A vendor false-positive report is the other option.
Only a code-signing certificate removes the heuristic entirely.

Build hardening was added in response (2026-10-21, behavior unchanged). The DLL
now has a `.rsrc` section built from `src/proxy/nioh3p.rc`. It holds a
`VERSIONINFO` block (ProductName `nioh3p`, FileDescription "DirectInput 8 proxy
(Nioh three-player co-op mod)", OriginalFilename `dinput8.dll`, version
1.0.0.0). It also holds an embedded application manifest (`RT_MANIFEST` id 1:
`asInvoker`, Windows 10/11 `supportedOS`, per-monitor DPI). A resource-less,
version-less unsigned DLL is a standard Evo-gen trigger. Code patches now
follow W^X. The metadata guard and the five visitor-allowance immediate writes
use `PAGE_READWRITE` for the store, then `FlushInstructionCache`, then an
explicit `PAGE_EXECUTE_READ`. No `VirtualProtect` call requests
`PAGE_EXECUTE_READWRITE` any more, and the constant survives only in read-only
`VirtualQuery` bitmask tests. The PE optional-header checksum, previously 0, is
recomputed by `tools/pechecksum.py` after the link. The log is opened with
`FILE_SHARE_READ`, so another process cannot rewrite it. The proxy logs
`build: resource manifest present` at startup as proof that the manifest
reached the linked image. These measures reduce the surface that the generic
heuristics look at. They cannot prove intent, so a fresh VirusTotal submission
is required before quoting a new detection count.

The rescan of the patched build (`9224d415…`) was blocked. That build has
VERSIONINFO and the `asInvoker` manifest in a `.rsrc` section, a real PE
checksum, no RWX (code pages go RW then RX), and the log opened share-read. The
anonymous web upload now asks for a reCAPTCHA image challenge, which this
session does not solve. The binary itself is unchanged in behavior and was
verified with three players in the rig. The host raised the lobby 2 -> 3, the
metadata guard installed at the second visitor's join, the log showed
`preflight: exe=ok eos=3/3 game_patch=ok`, and the manifest was reported as
present. The rescan then completed in a visible browser session, with no captcha
on that attempt. The patched build is still 5/71, with the same five generic
verdicts: Avast/AVG `Win64:Evo-gen`, Avira `TR/W64.Evo`, WithSecure
`Trojan.TR/W64.Evo` and Cynet `Malicious`. VirusTotal now recognizes it as a
normal PE32+ DLL (TrID "Win64 Executable (generic)"). Resources, manifest,
checksum and W^X therefore do not move these heuristics. The remaining levers
are code signing and vendor false-positive reports. Drafts are in
`docs/FP_REPORTS.md`. The four heuristic hits are Avast/AVG `Win64:Evo-gen`,
Avira `TR/W64.Evo`, WithSecure `Trojan.TR/W64.Evo` and Cynet. Only a
code-signing certificate or vendor false-positive reports are likely to clear
them, because the media they flag is "unsigned DLL that patches code".

Release scan of `1003e4e7…` (the build with stock lobby filters): 6/71. The five
earlier heuristic hits remain and Microsoft Defender added
`Trojan:Win32/Wacatac.C!ml`, its machine-learning generic. The detection list
changes on small binary changes, so treat the count as a moving number and the
vendor reports as the fix. Report: https://www.virustotal.com/gui/file/1003e4e73688f9e6af1bd95f3fbf71f5aeb3c756574b2ac27ecf1d7734f550cb

#### Windows 11 VM test of the player build `48415ff0…` (2026-10-21)

The VM is `dockurr/windows` (Windows 11 Pro 10.0.26200, KVM, NAT) with its
files in `~/nioh3p-test/winvm`. Defender ran at its defaults: real-time,
behavior and IOAV protection on, cloud reporting on, tamper protection on, and
no exclusions. The definitions were updated before the test to 1.461.144.0,
engine 1.1.26090.9. No Steam account was used.

| Step | Result |
|---|---|
| Copy and unzip `nioh3p-player.zip` | `dinput8.dll` kept, hash `48415FF0…`, 0 detections |
| Explorer-style unzip of a zip tagged with Mark of the Web (ZoneId 3) | Tag carried to all three files, 0 detections |
| Explicit Defender custom scan of the folder | 0 detections |
| Proxy harness `testgame.exe` with fake Steam and the real EOS SDK | All 6 PASS, `MaxLobbyMembers` clamped to 3, 0 detections |
| Real `nioh.exe` from the rig LAN copy (Steam and EOS emulators), run from a read-only share | Mod loaded: `preflight: exe=ok eos=3/3 steam=8 game_patch=ok`. Log fell back to `%TEMP%` as designed. 0 detections |
| Smart App Control set to On (registry, then reboot; `SmartAppControlState=On`) | The tagged `dinput8.dll` loaded in the harness (6 PASS) and in `nioh.exe`. Code Integrity logged no block event |

The game exits with code 53 in this VM after the mod loads. It exits with the
same code when `dinput8.dll` is absent, so the mod does not cause it. Code 53
is the game asking a running Steam client to start it (`steam://run/485510//`),
as on a cold direct launch in the rig (`RIG.md`, "Direct LAN launch status").
No Steam client runs in the VM, so gameplay there needs a Steam client. A
fresh Windows also lacks the Visual C++ runtime
and the DirectX June 2010 runtime (`XINPUT1_3.dll`, `D3DCOMPILER_43.dll`).
Steam installs both with Nioh. They were installed from Microsoft before the
game test.

Limits: one machine and one point in time. Defender's cloud verdict can change,
and the VirusTotal `Wacatac.C!ml` hit on the previous build shows that it does.
Smart App Control was switched on through the registry, not through the
Windows Security UI. The mission and multiplayer path did not run in the VM.

Release scan of `48415ff0…` (player build without research code, 38,400
bytes), uploaded through the VirusTotal web UI on 2026-10-22 with no captcha:
5/71. Avast and AVG `Win64:Evo-gen [Trj]`, Avira `TR/W64.Evo`, WithSecure
`Trojan.TR/W64.Evo` and Cynet `Malicious (score: 99)`. Microsoft reports it as
clean, so the `Wacatac.C!ml` verdict on `1003e4e7…` did not carry over. The CAPA
sandbox reported 0 malicious behavior. Zenbox and CAPE were still running at the
time of writing. Report:
https://www.virustotal.com/gui/file/48415ff045d11e34cb8e76ca9fc17afa2c8965b3d4f1c447ad96e01c646223a7

## 15. Four-player static research (2026-10-23)

The branch `four-player` asked whether the game code carries another limit
for a third visitor besides the allowance value. All addresses below were read in
`~/.cache/nioh3p/out/text.dis`. No client was run for this section.

### The shrine setter cap is one immediate byte

`0x140D5FB30` refuses its argument with `cmp $0x3,%ebx; jge` at `0x140D5FB4A`.
The immediate `0x03` sits at RVA `0xD5FB4C` inside the three bytes
`83 fb 03`. The compare immediate bounds the accepted range, so changing it
to `0x04` makes the setter accept 0 to 3. The store at
`0x140D5FB54` then writes the value to `root+0x39c` and the tail call at
`0x140D5FB67` runs the same propagation `0x140D7C950` that the direct stores
use. The setter has exactly two callers. The online shrine offering passes the
constant 1 at `0x140CF9F36`, which the proxy already rewrites. An item path
(`0x140CFA480`, item `0x74CC`) passes the constant 2 at `0x140CFA515`, and the
proxy leaves it alone. Raising the compare immediate does not touch either
caller.

### Consumption, admission and departure hold no second cap

The admission handler keeps its old shape at any allowance. It loads
`root+0x39c`, subtracts the admitted count from `0x140D79E50` and rejects with
reason 2 only when the signed delta is zero or less (`0x140D6B490`,
`0x140D6B4EA`). The accept path clears `root+0x39c` only when the delta was
exactly 1 (`0x140D6B51E`, `0x140D6B539`), so allowance 3 clears on the third
visitor the way allowance 2 cleared on the second. The departure path
decrements the allowance with a floor of zero (`0x140D7B222` to `0x140D7B234`)
and the close paths compare the allowance against a live count, never against
a constant (`0x140D7BCD2`, `0x140D7E2BA`).

### The slot table is four entries

Every slot loop in the session code runs while the index is below 4. The
admission counters scan non-host slots 1 to 3 (`0x140D79E97`, `0x140D7A227`),
the departure cleanup walks slots 0 to 3 (`0x140D7B186`), the admission tail
walks a four-entry bitmask (`0x140D6B60F`) and the recruitment-full scan walks
slots below 4 (`0x140D7BCCD`). The slots sit inline in the session root at
`0x141790A70` with stride `0xA8`, so slots 0 to 3 fill the first `0x2A0` bytes.
The table natively holds a host plus three visitors.

### The second member array is sized and read by the same limit

The create worker allocates the first member array with `limit` entries and a
second array with `limit - 2` entries only when `limit > 2`
(`0x1406049F0` to `0x140604A06`). The join preparation does the same from
`GetLobbyMemberLimit` (`0x140603D0B` to `0x140603D1F`). The second array at
`CSessionP2pSteam+0x1A8` is touched by four functions: two of them allocate
it, and `0x1405F7239` nulls it on teardown. Three of the four also read it:
`0x1405EF250`, `0x1405F2A3F` and `0x14060D62F`. All three readers compute
their loop bound as member count minus two and also guard each index against
the live member count at `+0x198`. None of them assumes a single entry, so a
limit of 4 with count 4 walks two entries of a two-entry array. Readers of the
first array at `+0x1A0` bound every index against the live count at `+0x198`
as well. No member array index is compared against a constant 2 or 3.

### What remains open

The mission layer and the packet layer are still untested beyond three
members. The
co-op partner calls are singular (`Online::GetCoopPartnerPlayerPosition`) and
the party HUD shows one name with three players. Only a live four-client run
can settle those paths.

## 16. Four players in one mission (2026-10-23)

Section 15 left the four-client question to a live run. This section
records that run. It took place on 2026-10-23 on branch `four-player`.
The player build from this branch has SHA-256 prefix `6db5351d` and is
38,400 bytes. `./build.sh` produced it.

### The code change and the smoke status

The proxy now also raises the shrine setter cap. It patches the byte at
RVA `0xd5fb4c`, the immediate of the cmp at `0xd5fb4a` (`cmp $0x3,%ebx`,
bytes `83 fb 03`), to 4 when the allowance is 3. The online shrine site can
then pass the value 3 past the setter's cap check. With allowance 2 the game
bytes stay stock.

The smoke harness passed with `max_players` 3 and 4, in player and
research mode, plus the ownership harness at both counts. The harness now
reads `max_players` from `nioh3p.ini` to build its expectation.
`NIOH3P_MAX_PLAYERS` appends an overriding `max_players` key in
`tools/smoketest.sh`.

### The three-player regression

Before the four-client run, the branch build passed a three-player
regression. Host b took visitors a and c, driven by
`coop.py run --host b --visitors a c`. All three roles passed. The host
logged `game create request: raising max members 2 -> 3 before
allocation`. Every client logged `game visitor allowance: recruitment
opens 2 visitor slot(s) (shrine setter 2)`. The host shot
`shots/4p-reg-3p-b.png` shows three player characters.

### The four-player run

All four lan slots then ran `max_players=4`. The three-player ini is
kept as `lan-*/nioh3p.ini.3p` in each slot. Every client logged `game
visitor allowance: recruitment opens 3 visitor slot(s) (shrine setter
3)`, so the setter cap raise installed on all four. Host b logged
`game create request: raising max members 2 -> 4 before allocation` at
13:44:01.

Visitors joined in order a, c and d, driven by
`coop.py run --host b --visitors a c d`. The driver relief (SIGSTOP pauses of
the waiting visitors) paused a and c while d loaded. a and c passed the
driver checks. d searched, found the lobby and sent JoinLobby at 13:56:28.
Three JoinLobby lines appear in its log. The driver then hit its default
600 s wall timeout while d was still loading. d finished loading on its own
and entered the host mission right after the driver timed out.

The evidence shots show four players in one mission. `shots/4p-mission-b.png`
and `shots/4p-move3-b.png` show the host view with the host and two visible
visitors; the third visitor is off camera (the driver trace records all
four). Visitor a's view with the host and the two other visitors is
`shots/4p-hostmove-a.png`. Movement was shown both ways. After a, c and
d each walked, `4p-move3-b.png` is the before frame and `4p-move-b.png` the
after frame; the visitor positions differ between them. After the host
walked, the visitor shots `4p-hostmove-a.png`, `4p-hostmove-c.png` and
`4p-hostmove-d.png` were taken. The driver trace is
`shots/coop-20261023-134257.jsonl` with 104 steps.

### Slot d identity and its save conversion

Slot d runs as SteamID `<steamid-lan-d>`, name `Nioh-LAN-D`, with an
Epic emulator id ending in 04 (epicid
`00000000000000000000000000000004`, productuserid
`<productuserid-lan-d>`) and save account `<accountid-d>`. The Nioh
Savedata Decryption Tool v1.3 converted slot c's `<accountid-c>` saves for
slot d. Run it as `-i <c file> -sid <accountid-d> -o SAVEDATA.BIN`. Use the
destination save folder as the working directory. The tool writes the output
and then waits on stdin. Give it Enter or stop it once the file exists.
While the tool runs, Proton also starts its Xalia helper process next to
the game.

### Rig script changes

`tools/rig/run.sh` accepts slot d for lan with the `compat-d` prefix
and slot b client files. `tools/rig/nioh_nav.py` lists a, b, c and d.
`tools/rig/coop.py` accepts two or three visitors, and `--relieve` now
takes several slots. `tools/rig/setup.sh` writes `max_players=4` into
slot a's ini on this branch.

### What remains open at four

The party HUD still names only one partner. The coop driver default
timeout of 600 s is too short for a fourth client's load. Pass
`--timeout 900` for four-player runs. Mission finish, deaths, revives,
rewards and a new offering after a visitor leaves are untested at
four. d's join needed the relief pauses on a and c.
