# EOS lobby call sites in the game image (static analysis)

Target: `~/.cache/nioh3p/dumps/nioh3p_image_merged.bin` (50,779,928 B runtime
image, **file offset == RVA**, VA = `0x140000000 + offset`, `SizeOfImage` 0x306D718).
All addresses below are VAs unless the row says `rva`; `rva X` == VA `0x140000000+X`.

Question answered here: *where does the game's EOS lobby path live, and is there an
EOS-side clamp that the current hooks miss (specifically
`EOS_LobbyModification_SetMaxMembers`)?*

**Headline result:** `EOS_LobbyModification_SetMaxMembers` is **not imported and not
referenced anywhere in the image** (no IAT slot, no name string, no code reference).
The game never calls it, so there is nothing for the current hook to patch, and the
"not hooked" log line is final, not a defect. The only EOS-side capacity input is
`EOS_Lobby_CreateLobby`'s `Options.MaxLobbyMembers` (slot `0x14119b1e0`, already
hooked), fed from the game's own session-config field `[rbx+4]`.

---

## 1. How the import table was recovered

The dump's PE headers are **zeroed** (first `0x1000` bytes are all `00`), so the
OptionalHeader data directories are unusable in this image. The import directory was
recovered from `.rdata` instead:

* The DLL-name string `EOSSDK-Win64-Shipping.dll` sits at rva `0x176385a`; the only
  dword in the image holding that RVA is `0x1761f14`, i.e. the `Name` field of a
  descriptor that starts at `0x1761f08`.
* Walking that 20-byte descriptor array backwards/forwards yields **20 descriptors**
  from `0x1761ee0` to `0x1762070` (null terminator at `0x1762070`).
* Cross-check: the recovered EOSSDK descriptor gives `EOS_Lobby_CreateLobby` the slot
  VA `0x14119b1e0`, which is exactly the slot the mod reports hooking, and its first
  call site rva is `0x5d915d`, exactly the frame the mod logs
  (`nioh.exe+0x5d915d`). The recovery is therefore verified against two independent
  observations.

Descriptor facts that matter for the diagnosis:

| # | descriptor | Name RVA | ILT (OFT) | IAT (FT) | entries |
|---|---|---|---|---|---|
| 2 | `EOSSDK-Win64-Shipping.dll` | `0x176385a` | `0x17620d0` | `0x119b048` | **124** |
| 0 | `steam_api64.dll` | `0x1762e74` | `0x1762d00` | `0x119bc78` | 11 |
| 8 | `KERNEL32.dll` | `0x1763ff8` | `0x1762520` | `0x119b498` | 163 |

* There is **exactly one** descriptor for the EOS SDK (no duplicate under a second
  name, e.g. no `EOSSDK-Win64-Shipping.dll` + `EOSSDK-Win64-Shipping` pair).
* Its ILT contains **no ordinal entries** — all 124 entries are hint/name RVAs
  (`EOS_Lobby_...`, `EOS_Platform_...`, `EOS_P2P_...`, …). Nothing is imported by
  ordinal.
* The DLL name string is byte-identical to the name the proxy asks for, so the
  proxy's `stricmp_a(dll, "EOSSDK-Win64-Shipping.dll")` match succeeds (that is what
  the log's `present=1` means — see §7).
* EOSSDK IAT span: `0x14119b048` … `0x14119b420` (124 × 8 bytes).
* Every IAT in the module is packed at the start of the section that follows `.text`:
  the lowest `FT` is `0x119b000` (ADVAPI32, a 4 KiB boundary) and `.text` ends at
  `0x119a100`. That region also holds the import name strings (`0x176385a`) and the
  RTTI data (`nioh3p.sites.txt` vtables ≤ `0x1588620`), i.e. it is `.rdata` —
  read-only at runtime.
* For the five headline imports, the EOS export address occurs **exactly once** in the
  whole 50 MB image, at its IAT slot:

  | import | export address in slot | occurrences of that qword |
  |---|---|---|
  | `EOS_Lobby_CreateLobby` | `0x6ffffc164340` | `0x119b1e0` only |
  | `EOS_Lobby_JoinLobby` | `0x6ffffc164470` | `0x119b2b8` only |
  | `EOS_LobbyModification_AddAttribute` | `0x6ffffc164560` | `0x119b2a0` only |
  | `EOS_LobbyDetails_GetMemberCount` | `0x6ffffc1644e0` | `0x119b080` only |
  | `EOS_LobbyDetails_CopyInfo` | `0x6ffffc164680` | `0x119b2e0` only |

  So the game reaches EOS only through the IAT; it keeps no second copy of any EOS
  pointer.

---

## 2. The game's EOS lobby imports (name → IAT slot VA, the hook target)

All 40 `EOS_Lobby*` / `EOS_LobbyModification*` / `EOS_LobbyDetails*` /
`EOS_LobbySearch*` imports of descriptor #2, with the slot the mod rewrites, and the
number of references found in the image (`call` = `ff 15` call-indirect to the slot,
`thunk` = `ff 25` tail-jump stub in the table at `0x14116dd69`, `other` = a
coincidental match inside data, i.e. not a real reference).

| import | IAT slot VA | call refs | thunk | other |
|---|---|---|---|---|
| `EOS_LobbySearch_Release` | `0x14119b048` | 1 | 1 | 1 |
| `EOS_LobbyDetails_GetLobbyOwner` | `0x14119b050` | 4 | 1 | 0 |
| `EOS_LobbySearch_Find` | `0x14119b058` | 1 | 1 | 0 |
| `EOS_LobbySearch_CopySearchResultByIndex` | `0x14119b060` | 1 | 1 | 1 |
| `EOS_LobbySearch_GetSearchResultCount` | `0x14119b068` | 1 | 1 | 0 |
| `EOS_LobbySearch_SetParameter` | `0x14119b070` | 5 | 1 | 0 |
| `EOS_LobbyDetails_Info_Release` | `0x14119b078` | 2 | 1 | 0 |
| `EOS_LobbyDetails_GetMemberCount` | `0x14119b080` | 2 | 1 | 0 |
| `EOS_Lobby_Attribute_Release` | `0x14119b088` | 6 | 1 | 0 |
| `EOS_Lobby_CreateLobbySearch` | `0x14119b090` | 1 | 1 | 1 |
| `EOS_Lobby_SendInvite` | `0x14119b098` | 6 | 1 | 1 |
| `EOS_Lobby_RejectInvite` | `0x14119b0a0` | 6 | 1 | 2 |
| `EOS_LobbyModification_Release` | `0x14119b0a8` | 5 | 1 | 0 |
| `EOS_Lobby_CopyLobbyDetailsHandle` | `0x14119b178` | 6 | 1 | 0 |
| `EOS_Lobby_KickMember` | `0x14119b1b8` | 6 | 1 | 0 |
| `EOS_Lobby_CreateLobby` | `0x14119b1e0` | 6 | 1 | 0 |
| `EOS_LobbyModification_AddAttribute` | `0x14119b2a0` | 19 | 1 | 0 |
| `EOS_Lobby_UpdateLobby` | `0x14119b2a8` | 10 | 1 | 0 |
| `EOS_Lobby_UpdateLobbyModification` | `0x14119b2b0` | 10 | 1 | 0 |
| `EOS_Lobby_JoinLobby` | `0x14119b2b8` | 6 | 1 | 1 |
| `EOS_Lobby_DestroyLobby` | `0x14119b2c8` | 3 | 1 | 0 |
| `EOS_Lobby_LeaveLobby` | `0x14119b2d0` | 6 | 1 | 0 |
| `EOS_Lobby_RemoveNotifyLobbyInviteReceived` | `0x14119b2d8` | 2 | 1 | 0 |
| `EOS_LobbyDetails_CopyInfo` | `0x14119b2e0` | 20 | 1 | 0 |
| `EOS_Lobby_CopyLobbyDetailsHandleByUiEventId` | `0x14119b2e8` | 6 | 1 | 0 |
| `EOS_Lobby_RemoveNotifyLobbyInviteAccepted` | `0x14119b2f0` | 2 | 1 | 0 |
| `EOS_LobbyDetails_CopyAttributeByKey` | `0x14119b2f8` | 13 | 1 | 0 |
| `EOS_Lobby_AddNotifyLobbyInviteAccepted` | `0x14119b300` | 4 | 1 | 0 |
| `EOS_Lobby_CopyLobbyDetailsHandleByInviteId` | `0x14119b308` | 12 | 1 | 0 |
| `EOS_Lobby_AddNotifyJoinLobbyAccepted` | `0x14119b310` | 4 | 1 | 0 |
| `EOS_Lobby_RemoveNotifyJoinLobbyAccepted` | `0x14119b318` | 2 | 1 | 0 |
| `EOS_LobbyDetails_Release` | `0x14119b328` | 29 | 1 | 0 |
| `EOS_Lobby_AddNotifyLobbyInviteReceived` | `0x14119b330` | 4 | 1 | 0 |
| `EOS_LobbyDetails_GetMemberByIndex` | `0x14119b340` | 1 | 1 | 0 |
| `EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived` | `0x14119b368` | 2 | 1 | 0 |
| `EOS_Lobby_AddNotifyLobbyUpdateReceived` | `0x14119b370` | 1 | 1 | 0 |
| `EOS_Lobby_AddNotifyLobbyMemberStatusReceived` | `0x14119b378` | 1 | 1 | 0 |
| `EOS_Lobby_RemoveNotifyLobbyUpdateReceived` | `0x14119b380` | 2 | 1 | 0 |
| `EOS_Lobby_PromoteMember` | `0x14119b3a0` | 2 | 1 | 1 |

One more EOS lobby entry point outside those prefixes, needed below:

| import | IAT slot VA | call refs |
|---|---|---|
| `EOS_Platform_GetLobbyInterface` | `0x14119b320` | 81 |

`EOS_Platform_GetLobbyInterface` is the entry every lobby object is obtained from; the
81 call sites are the pattern "get the lobby interface, then call a lobby API". The
remaining 83 EOSSDK imports are the other interfaces (`EOS_Platform_*`,
`EOS_Connect_*`, `EOS_Auth_*`, `EOS_P2P_*`, `EOS_Presence_*`, `EOS_Stats_*`,
`EOS_Achievements_*`, `EOS_Leaderboards_*`, `EOS_Friends_*`, `EOS_Ecom_*`,
`EOS_UI_*`) plus the `*_Release` helpers.

---

## 3. `EOS_LobbyModification_SetMaxMembers`: no slot, no reference

Scan of the whole 50 MB image (single pass, `rip`-relative decode of every dword):

* ILT of the EOSSDK descriptor (`0x17620d0`, 124 entries): **no entry named
  `EOS_LobbyModification_SetMaxMembers`**. The only `EOS_LobbyModification_*`
  imports are `AddAttribute` (slot `0x14119b2a0`) and `Release` (slot `0x14119b0a8`).
* Name strings: `EOS_LobbyModification_` occurs exactly **twice** in the image, at
  `0x176476a` (`AddAttribute`) and `0x1764790` (`Release`). The substring
  `SetMaxMembers` occurs **zero** times anywhere in the image.
* Other lobby-modification setters are likewise absent: `SetPermissionLevel`,
  `SetBucketId`, `SetAllowedPlatformIds`, `SetJoinable` — 0 occurrences each.
* Therefore: **there is no IAT slot VA for `EOS_LobbyModification_SetMaxMembers` in
  this build**, and **there are zero RIP-relative references to it** — no
  `ff 15` call, no `ff 25` tail-jump, no `48 8b 05/0d/15/1d` load, because there is
  no address to reference. The game cannot call a function it never resolves.

Consequence: the "EOS-side clamping gap" on the modification path does not exist in
this build. The only EOS-side capacity input is the `MaxLobbyMembers` field of the
`EOS_Lobby_CreateLobby` options, which the mod already rewrites (§7 item 1).

Scan coverage note (applies to §4 as well): for all 40 lobby-family imports the
reference scan found **220 `ff 15` calls, 39 `ff 25` stubs and 8 coincidental data
matches — and zero `48 8b 05/0d/15/1d`-style loads, `lea`s or stores of any slot**. The
game never copies an EOS entry point into a register or a table, so there is no
indirect/dynamic dispatch path that an IAT hook could miss (and no place where a
never-imported function such as `SetMaxMembers` could be reached either).

---

## 4. Reference scan for the five hooked functions

Method: every dword `D` at offset `o` with `VA(o)+4+D == slot VA` and an opcode prefix
of `ff 15` / `ff 25` counts as a call site (instruction start = `o-2`); the enclosing
function is taken from the `.pdata` table at rva `0x2ee1000` (72,232 entries), with
contiguous fragment entries coalesced; every reported function start is preceded by
`cc` padding (checked). Names are the exact `nioh3p.sites.txt` entry when the function
start is listed there, otherwise `sub_<rva>`; the nearest RTTI method *below* is shown
for context only (the RTTI map only covers virtual methods, so most of the EOS code is
unnamed).

### 4.1 `EOS_Lobby_CreateLobby` — slot `0x14119b1e0`, 6 call sites

| call site rva | enclosing function | sites.txt name / context |
|---|---|---|
| `0x5d3dbd` | `0x5d3cc0-0x5d3e3f` | `sub_5D3CC0` (below `ktol::sessionp2p::eos::session::SessionService::vfn2` @ `0x5d3780`) |
| `0x5d75be` | `0x5d74c0-0x5d7640` | `?$_Func_impl_no_alloc@V<lambda_44d22d624fb5c50be243eef43590a7a3>@@H$$V@std::vfn2` |
| `0x5d85dd` | `0x5d84e0-0x5d865f` | `sub_5D84E0` |
| **`0x5d915d`** | `0x5d9060-0x5d91df` | `sub_5D9060` — **the site the mod logs** |
| `0x5d9bfd` | `0x5d9b00-0x5d9c7f` | `sub_5D9B00` |
| `0x5de0a0` | `0x5ddfb0-0x5de10b` | `sub_5DDFB0` (below `...task::GrantOwnerTask::vfn3` @ `0x5ddc70`) |

Plus one `ff 25` tail-jump at `0x116de2f` (thunk table, §6.2).

All six sites build an *identical* `EOS_Lobby_CreateLobbyOptions` at `rsp+0x20`:
`ApiVersion = 7` (immediate `movq $0x7,0x20(%rsp)` at rva `0x5d3d36`, `0x5d7537`,
`0x5d8556`, `0x5d90d6`, `0x5d9b76`, `0x5de01a`), `LocalUserId = [global+0x48]`,
`MaxLobbyMembers = [rbx+4]` (the game's own session-config field),
`PermissionLevel = 2 or 0`, `bPresenceEnabled = ([rbx+0x6c] != 0)`,
`bAllowInvites = 0`, `BucketId = "Nioh"` (string at `0x1415864ec`).

The logged site, rva `0x5d915d` in `sub_5D9060` (`0x1405d90fd`-`0x1405d9163`):

```
1405d90fd: mov  0x4(%rbx),%eax              ; game-side max players
1405d9101: lea  0xfad3e4(%rip),%rax         # 0x1415864ec "Nioh"
1405d9108: mov  %rax,0x40(%rsp)             ; options.BucketId   (opt+0x20)
1405d9123: mov  %ecx,0x34(%rsp)             ; options.PermissionLevel (0 or 2)
1405d9152: mov  0x58(%rdi),%rcx             ; EOS_HLobby handle
1405d915d: call *0xbc207d(%rip)             # 0x14119b1e0  EOS_Lobby_CreateLobby
```

`MaxLobbyMembers` is written to `options+0x10` = `0x30(%rsp)` two instructions earlier
(`mov 0x4(%rbx),%eax; mov %eax,0x30(%rsp)`). That field is sufficient to raise
the EOS **service** capacity. It does not establish native member-array sizing
or third-player gameplay; the Steam host trace demonstrates why those are separate.

### 4.2 `EOS_Lobby_JoinLobby` — slot `0x14119b2b8`, 6 call sites

| call site rva | enclosing function | context |
|---|---|---|
| `0x5d3ff2` | `0x5d3f20-0x5d4077` | `sub_5D3F20` (below `SessionService::vfn2` @ `0x5d3780`) |
| `0x5d7753` | `0x5d7680-0x5d77d8` | `?$_Func_impl_no_alloc@V<lambda_540333a9abed8c049c6772f41df04da2>@@H$$V@std::vfn2` |
| `0x5d8742` | `0x5d8670-0x5d87c7` | `sub_5D8670` |
| `0x5d92c2` | `0x5d91f0-0x5d9347` | `sub_5D91F0` |
| `0x5d9d52` | `0x5d9c80-0x5d9dd7` | `sub_5D9C80` |
| `0x5dcc8a` | `0x5dcbc0-0x5dccf1` | `sub_5DCBC0` (below `...task::LockTask::vfn3` @ `0x5dc950`) |

Plus tail-jump at `0x116de17`. All six set `JoinLobbyOptions.ApiVersion = 3`
(immediate at rva `0x5d3f7a`, `0x5d76db`, `0x5d86ca`, `0x5d924a`, `0x5d9cda`,
`0x5dcc15`), `bPresenceEnabled` from the same flag, e.g. rva `0x5d3f40`-`0x5d3ff2`:

```
1405d3f44: call *0xbc73d5(%rip)   # 0x14119b320  EOS_Platform_GetLobbyInterface
1405d3f7a: movq $0x3,0x20(%rsp)   # JoinLobbyOptions.ApiVersion = 3
1405d3fb5: setne %al              # bPresenceEnabled
1405d3ff2: call *...              # 0x14119b2b8  EOS_Lobby_JoinLobby
```

### 4.3 `EOS_LobbyModification_AddAttribute` — slot `0x14119b2a0`, 19 call sites

| call site rva | enclosing function | context |
|---|---|---|
| `0x5d4e5b` | `0x5d4d40-0x5d4f09` | `sub_5D4D40` — the lock/unlock ladder (§6.3) |
| `0x5d79fc` | `0x5d78e0-0x5d7aaa` | `?$_Func_impl_no_alloc@V<lambda_33ed775b4d1a6af745056c7d82908b63>@@H$$V@std::vfn2` |
| `0x5d895b` | `0x5d8840-0x5d8a09` | `sub_5D8840` |
| `0x5d94db` | `0x5d93c0-0x5d9589` | `sub_5D93C0` |
| `0x5d9f3b` | `0x5d9e20-0x5d9fe9` | `sub_5D9E20` |
| `0x5dadd0`, `0x5daec7` | `0x5dac50-0x5dafb7` | `sub_5DAC50` (below `...task::NotifyLostTask::vfn3` @ `0x5dac10`) |
| `0x5db1ce` | `0x5db0f0-0x5db236` | `sub_5DB0F0` (below `...task::ModifyAttributeTask::vfn3` @ `0x5db0a0`) |
| `0x5db296` | `0x5db240-0x5db2aa` | `sub_5DB240` |
| `0x5dc791` | `0x5dc690-0x5dc82e` | `sub_5DC690` (below `...task::SearchTask::vfn3` @ `0x5dbfc0`) |
| `0x5de877`, `0x5de8cf`, `0x5de999`, `0x5deab6`, `0x5deba7` | `0x5de770-0x5dec6e` | `sub_5DE770` (below `...task::CreateTask::vfn3` @ `0x5de700`) |
| `0x5dedae` | `0x5decd0-0x5dee16` | `sub_5DECD0` |
| `0x5dee76` | `0x5dee20-0x5dee8a` | `sub_5DEE20` |
| `0x5e4095` | `0x5e3e20-0x5e4107` | `sub_5E3E20` |
| `0x5e4297` | `0x5e41c0-0x5e42db` | `sub_5E41C0` |

Plus tail-jump at `0x116ddbd`. Attribute keys used near these calls are
`ktol_lobby_lock` (`0x141563e80`), `ktol_lobby_unlock` (`0x141563e68`) and
`ktol_lobby_lock_status` (`0x141563e90`).

### 4.4 `EOS_LobbyDetails_GetMemberCount` — slot `0x14119b080`, 2 call sites

| call site rva | enclosing function | context |
|---|---|---|
| `0x5dc191` | `0x5dc030-0x5dc5ce` | `sub_5DC030` — lobby read-back (§6.4) |
| `0x5dd029` | `0x5dcf00-0x5dd095` | `sub_5DCF00` (below `...task::JoinTask::vfn3` @ `0x5dceb0`) |

Plus tail-jump at `0x116dde1`. `sub_5DCF00` is the member enumeration: get owner,
count, then loop `GetMemberByIndex` (`0x1405dd050`-`0x1405dd07b`).

### 4.5 `EOS_LobbyDetails_CopyInfo` — slot `0x14119b2e0`, 20 call sites

| call site rva | enclosing function |
|---|---|
| `0x5ce16e` | `0x5ce0e0-0x5ce1e3` `sub_5CE0E0` |
| `0x5ce2d2` | `0x5ce240-0x5ce346` `sub_5CE240` |
| `0x5ce42d` | `0x5ce390-0x5ce4a5` `sub_5CE390` |
| `0x5d029f` | `0x5d0210-0x5d0314` `?$_Func_impl_no_alloc@V<lambda_a076af67c9803a2061b943b4b7e5a85d>@@XPEAVSessionService@session@eos@sessionp2p@ktol@@@std::vfn2` |
| `0x5d0403` | `0x5d0370-0x5d0477` `...<lambda_dbfb7934a7c62586ac26e6f720c36aeb>...::vfn2` |
| `0x5d055e` | `0x5d04c0-0x5d05d6` `...<lambda_8b046903fbe7765e76a0260f66b3da07>...::vfn2` |
| `0x5d13ae` | `0x5d1320-0x5d1423` `sub_5D1320` |
| `0x5d14e2` | `0x5d1450-0x5d1556` `sub_5D1450` |
| `0x5d160d` | `0x5d1570-0x5d1685` `sub_5D1570` |
| `0x5d1f8e` | `0x5d1f00-0x5d2003` `sub_5D1F00` |
| `0x5d20c2` | `0x5d2030-0x5d2136` `sub_5D2030` |
| `0x5d21ed` | `0x5d2150-0x5d2265` `sub_5D2150` |
| `0x5d294e` | `0x5d28c0-0x5d29c3` `sub_5D28C0` |
| `0x5d2a72` | `0x5d29e0-0x5d2ae6` `sub_5D29E0` |
| `0x5d2b8d` | `0x5d2af0-0x5d2c05` `sub_5D2AF0` |
| `0x5d64ad` | `0x5d6420-0x5d6522` `sub_5D6420` |
| `0x5d65d2` | `0x5d6540-0x5d6646` `sub_5D6540` |
| `0x5d66ed` | `0x5d6650-0x5d6765` `sub_5D6650` |
| `0x5dc152` | `0x5dc030-0x5dc5ce` `sub_5DC030` — lobby read-back (§6.4) |
| `0x5dcfb6` | `0x5dcf00-0x5dcfdb` `sub_5DCF00` |

Plus tail-jump at `0x116dd99`. Most of these are the "copy the lobby-details handle
from an invite / UI event, then `CopyInfo`" pair (`CopyLobbyDetailsHandleByInviteId`
at `0x14119b308` or `CopyLobbyDetailsHandleByUiEventId` at `0x14119b2e8` immediately
precedes `CopyInfo` in the same function, e.g. `0x5ce3fa` then `0x5ce42d` in
`sub_5CE390`).

> Note on the slot arithmetic: `EOS_LobbyDetails_CopyInfo` is ILT index 83, i.e. slot
> `0x14119b048 + 83*8 = 0x14119b2e0`. `0x14119b2e8` is
> `EOS_Lobby_CopyLobbyDetailsHandleByUiEventId`. A one-slot slip here silently
> mis-attributes 6 of the CopyInfo hits.

---

## 5. Why the `SetMaxMembers` hook fails while the others work

The proxy (`src/proxy/nioh3p_proxy.c`):

* `find_iat_slot()` walks the descriptor array, matches the DLL name, sets
  `*saw_iat = 1`, then searches the ILT for the exact symbol name. The log prints
  `present=%d` with `saw_iat`, so **`present=1` only means "the EOSSDK descriptor
  exists"** — it does not mean the symbol was found. `SetMaxMembers` is not in the
  ILT, so the function returns `NULL` and the fallback runs.
* `scan_and_patch_range()` is the fallback: it walks the main module and patches every
  qword equal to the resolved EOS entry point, but **only in regions whose protection
  is `PAGE_READWRITE`, `PAGE_WRITECOPY`, `PAGE_EXECUTE_READWRITE` or
  `PAGE_EXECUTE_WRITECOPY`**. For EOS that is structurally the wrong place to look:
  the only qword equal to an EOS export address in the image is the IAT slot itself
  (§1, "occurrences"), and all IATs live in the read-only `.rdata` block at
  `0x119b000+`. Hence `pointer scan patched 0 location(s)`, and for
  `SetMaxMembers` there is no such qword at all (the export is never resolved into the
  game).

The hypotheses in the ticket are all *not* the cause, and the import table shows why:

| hypothesis | finding |
|---|---|
| duplicate import (two descriptors) | only one descriptor names the EOS SDK (descriptor #2 @ `0x1761f08`); the other 19 names are `steam_api64.dll`, `Kernel32.dll`, … |
| different DLL name in the descriptor | the descriptor's `Name` is byte-exactly `EOSSDK-Win64-Shipping.dll` (`0x176385a`) |
| ordinal import | the EOSSDK ILT (`0x17620d0`, 124 entries) contains **no** `IMAGE_ORDINAL_FLAG64` entries |
| tail call through a different register | not applicable: the game calls EOS through the IAT with `ff 15` (and via `ff 25` stubs that read the same slots); the export address exists nowhere else in the image |
| the protector rebuilt the IAT | not here: each slot holds the real EOS export address (`0x6ffff...`), which is why the five name-based hooks succeed and the proxy logs "hooked through the import table" |

Root cause: **the game does not import `EOS_LobbyModification_SetMaxMembers` at all.**
No hook mechanism can find a call site that does not exist; the log line
`NOT hooked (no call site found in this build)` is the correct, final answer for this
build.

---

## 6. Shape of the EOS session path (context for the clamp)

### 6.1 Six copies of the session code

`CreateLobby` (6 sites), `JoinLobby` (6 sites) and `CopyInfo` (20 sites in ~6
repeating blocks) are **duplicated near-verbatim** in `.text` rva
`0x5c0f90`-`0x5e4300`. Example: `sub_5D3CC0` and `sub_5D9060` contain byte-identical
option-building sequences (`0x5d3d7c`/`0x5d911c` `and $0x2,%ecx`, …). The copies are
`ktol::sessionp2p::eos::session::task::*` task bodies plus
`std::_Func_impl_no_alloc<lambda_…>::vfn2` callback bodies (see `nioh3p.sites.txt`).
Because the mod patches the **IAT slot**, one hook covers all copies — verified: all 6
`CreateLobby` call sites load from `0x14119b1e0`.

### 6.2 Tail-jump stub block at the end of `.text`

`.text` ends with a contiguous block of 63 `ff 25` tail-jumps, rva
`0x116dd51` … `0x116dec5` (preceded by a small function ending in `ret` at
`0x116dd50`, followed by `cc` padding), forwarding to 63 distinct IAT slots — the EOS
lobby/detail/search slots plus one `steam_api64.dll` slot (`0x14119bc78`):

```
14116ddbd: jmp *0x2d4dd(%rip)   # 0x14119b2a0  EOS_LobbyModification_AddAttribute
14116de17: jmp *0x2d49b(%rip)   # 0x14119b2b8  EOS_Lobby_JoinLobby
14116de2f: jmp *0x2d3ab(%rip)   # 0x14119b1e0  EOS_Lobby_CreateLobby
```

A scan of the image for references into that block (rel32 `E8`/`E9`, `rip`-relative
`call`/`jmp`/`lea`, or 8-byte pointers) found **none** — the stubs are not referenced
statically, so nothing in the game enters them today. They are harmless for hooking in
any case: they re-read the same IAT slots, so patching the slot covers them too (this
is why one IAT hook covers all six `CreateLobby` call sites *and* this stub).

### 6.3 The lobby-modification ladder (where `SetMaxMembers` *would* sit)

`sub_5D4D40` (`0x5d4d40-0x5d4f09`, below `SessionService::vfn2` @ `0x5d3780`) is the
"modify lobby attribute" task. It performs the complete ladder and never calls a
`Set*`:

```
1405d4deb: call *0xbc64bf(%rip)   # 0x14119b2b0  EOS_Lobby_UpdateLobbyModification
1405d4e5b: call *0xbc643f(%rip)   # 0x14119b2a0  EOS_LobbyModification_AddAttribute
1405d4ec5: call *0xbc63dd(%rip)   # 0x14119b2a8  EOS_Lobby_UpdateLobby
```

The attribute key is `ktol_lobby_lock` / `ktol_lobby_unlock` (chosen with `cmovne` at
`0x5d4e37`, strings at `0x141563e80` / `0x141563e68`) plus a
`ktol_lobby_lock_status` string at `0x141563e90`. Between the modification object's
creation and `UpdateLobby` there is exactly one `EOS_LobbyModification_*` call, and it
is `AddAttribute` — the modification object is never asked to change capacity.

### 6.4 The read-back that feeds the game's own lobby state

`sub_5DC030` (`0x5dc030-0x5dc5ce`) refreshes the game's lobby state from EOS:

```
1405dc152: call *0xbbf188(%rip)   # 0x14119b2e0  EOS_LobbyDetails_CopyInfo
1405dc177: mov  0x20(%rax),%r15d  # Info->MaxMembers (EOS_LobbyDetails_Info+0x20)
1405dc191: call *0xbbeee9(%rip)   # 0x14119b080  EOS_LobbyDetails_GetMemberCount
1405dc19d: test %eax,%eax
1405dc19f: jle  0x1405dc588      # no members -> skip enumeration
1405dc1b7: call *0xbbee93(%rip)   # 0x14119b050  EOS_LobbyDetails_GetLobbyOwner
```

`0x20` is `MaxMembers` per the mod's own `LobbyDetailsInfo`
(`src/proxy/nioh3p_proxy.c`, "MaxMembers /* 32 */"); `sub_5DCF00` (`0x5dcf00`) does
the same for the join path, then loops `GetMemberByIndex` over the count. These two
functions are the only places where the game converts EOS capacity/membership into
its own state — i.e. the natural place to observe a *local* clamp.

---

## 7. Recommended fix (concrete addresses)

1. **Keep the `EOS_Lobby_CreateLobby` hook** at slot `0x14119b1e0`. It is the only
   EOS-side capacity input; `Options.MaxLobbyMembers` is at `options+0x10` and is
   copied from the game's session-config field `[rbx+4]` (store at rva `0x5d90fd` for
   the logged site; the other five copies do the same). Rewriting it to
   `max_players` covers the EOS service limit, not proof of native gameplay capacity.
2. **`EOS_LobbyModification_SetMaxMembers`: nothing to patch in this build.** There is
   no IAT slot and no call site. Do not add a slot-scan for it; the correct log line is
   the current `NOT hooked (no call site found in this build)`.
3. **If the fallback is to be made useful at all**, make `scan_and_patch_range()`
   include read-only regions (`PAGE_READONLY`, `PAGE_EXECUTE_READ`) — the current
   writable-only filter can never see an IAT slot, so that fallback is dead code for
   every EOS import (and for `SetMaxMembers` there is no pointer to find even then).
   This is the only *structural* defect the investigation turned up.
4. **Optional extra hook points** (all existing slots, so no new code paths needed):
   * `0x14119b2e0` `EOS_LobbyDetails_CopyInfo` and `0x14119b080`
     `EOS_LobbyDetails_GetMemberCount` — the read-back in `sub_5DC030`
     (`0x5dc152`, `0x5dc191`) and `sub_5DCF00` (`0x5dcfb6`, `0x5dd029`) is where the
     game's own `MaxMembers`/member count is produced.
   * `0x14119b2b0` `EOS_Lobby_UpdateLobbyModification`, `0x14119b2a0`
     `EOS_LobbyModification_AddAttribute`, `0x14119b2a8` `EOS_Lobby_UpdateLobby` — the
     ladder in `sub_5D4D40` (`0x5d4deb`, `0x5d4e5b`, `0x5d4ec5`) is the only place a
     capacity change *could* be issued after lobby creation.
   * If a code patch (rather than an API hook) is ever wanted, the exact instruction
     to change is the `MaxLobbyMembers` store `mov %eax,0x30(%rsp)` (options+0x10,
     source `mov 0x4(%rbx),%eax`). All six sites, load address → store address:
     `0x5d3d5a`→`0x5d3d5d`, `0x5d755b`→`0x5d755e`, `0x5d857a`→`0x5d857d`,
     `0x5d90fa`→`0x5d90fd` (the logged site `0x5d915d`), `0x5d9b9a`→`0x5d9b9d`,
     `0x5de039`→`0x5de03c`.

---

## 8. Log text vs image (flagged discrepancy)

The README's sample log line

```
[...] EOS_Lobby_CreateLobby: api=3 max=2 perm=1 presence=1 invites=1 bucket=Nioh
```

does not match the image: every `CreateLobby` site here builds `ApiVersion = 7`,
`PermissionLevel` 0 or 2, `bAllowInvites = 0`, `BucketId = "Nioh"` (§4.1).
`api=3 / perm=1 / presence=1 / invites=1` are exactly the values the *test harness*
sets (`tools/testgame/testgame.c` lines 172-178: `ApiVersion = 3`,
`MaxLobbyMembers = 2`, `PermissionLevel = 1`, `bPresenceEnabled = 1`,
`bAllowInvites = 1`), so **[INFERENCE]** that README line looks like a composite of a
test-harness run and a game run. The one part of it that the image confirms is the
frame address `nioh.exe+0x5d915d`, which is a real `EOS_Lobby_CreateLobby` call site
(§4.1). The second frame `nioh.exe+0x5e27a1` lies inside `sub_5E2789`
(`0x5e2789-0x5e2a5d`), which reads lobby attributes (`CopyAttributeByKey` at `0x5e2861`,
`0x5e28f7`) — a task-execute frame above the create path, not a second capacity site.

---

## 9. Reproduction

```sh
# import table (headers are zeroed in the dump -> reconstruct from .rdata)
python3 - <<'EOF'
import os, struct
d = open(os.path.expanduser('~/.cache/nioh3p/dumps/nioh3p_image_merged.bin'),'rb').read()
oft, _, _, nm, ft = struct.unpack_from('<IIIII', d, 0x1761f08)   # EOSSDK descriptor
i = 0
while (v := struct.unpack_from('<Q', d, oft + 8*i)[0]):
    e = d.index(b'\0', v+2)
    print('%3d 0x%08x  %s' % (i, 0x140000000 + ft + 8*i, d[v+2:e].decode()))
    i += 1
EOF

# a call site window (recipe from the task ticket)
nice -n 10 objdump -D -b binary -m i386:x86-64 --adjust-vma=0x140000000 \
  "$HOME/.cache/nioh3p/dumps/nioh3p_image_merged.bin" \
  --start-address=0x1405d90c0 --stop-address=0x1405d91a0
```

`.pdata` (function boundaries used above) is at rva `0x2ee1000`: 72,232 × 12-byte
`RUNTIME_FUNCTION` entries (`Begin`, `End`, `UnwindInfo`) followed by a zero
terminator at rva `0x2fb49e0`; the last entry begins at rva `0x119a444`, i.e. the
table covers exactly `.text` (`0x1000`-`0x119a100`, matching `FINDINGS.md` §2:
`.text` 18,449,408 B). Some `End` fields in that table are mangled (e.g. entry #2390
`0x1050a0`-`0x12420d`), so only the `Begin` values were used; each reported function
start was validated by the `cc` padding that precedes it.
