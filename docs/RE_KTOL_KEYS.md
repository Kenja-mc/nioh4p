# RE: the `ktol_*` lobby-data keys — invite / lock / join gate

Offline static analysis of the runtime image. Read-only: nothing in the game or
the mod was run, built or touched. Re-runnable with `tools/ktol_refs.py`.

## 0. Method and what is trusted

| Fact used | Evidence |
|---|---|
| Image = `~/.cache/nioh3p/dumps/nioh3p_image_merged.bin`, 50,779,928 B, `offset == RVA`, `VA = 0x140000000 + offset` | dossier; re-checked: `.pdata` at RVA 0x2EE1000 parses as 72,232 `RUNTIME_FUNCTION` records, **all** `begin < end` inside `.text` (0x1000..0x119A498) |
| Function boundaries are real | every "function RVA" below is a `.pdata` `begin`. Independently matching: two of them (`0x5FA3E0`, `0x5FB560`) are listed as vtable methods in `analysis/classmap.txt` |
| Import names are real | the `IMAGE_IMPORT_DESCRIPTOR` array in `.rdata` (0x1761EE0..) is intact; only the IAT is patched with resolved addresses (slot 0x14119B2A0 holds `0x6FFFFC164560`, i.e. a loaded-module pointer). Slot → name mapping used throughout, e.g. `0x14119B2A0 = EOS_LobbyModification_AddAttribute`, `0x14119B2B8 = EOS_Lobby_JoinLobby` |
| `.text` is plaintext here (unlike the on-disk exe) | `objdump` at the mod's own backtrace frames decodes cleanly (e.g. `0x1405EDCD0: push %rbx; sub $0x20,%rsp`) |
| `analysis/symbols.txt` | **is a name pool without addresses** (see `tools/symtab.py` docstring: "Addresses are *not* in this table: it is a name pool"). It corroborates *names* only. Class↔code address pairs come from `analysis/classmap.txt` |
| Live log | `~/.cache/nioh3p/out/nioh3p.log` line 161 `Steam SetLobbyData: key="ktol_lock_status" value="ktol_session_unlock"` is used as the ground-truth anchor for key direction |

Reproduce any line of this report with:

```
tools/ktol_refs.py strings                      # key literals + RVAs
tools/ktol_refs.py xrefs ktol_session_lock      # lea xrefs -> enclosing .pdata function
tools/ktol_refs.py ptrs                         # 8-byte data-pointer xrefs
tools/ktol_refs.py dis 0x1405fb680 0x40        # annotated window (IAT + key names)
tools/ktol_refs.py callers 0x1405fb560          # direct call sites
```

## 1. The key literals

String scan over the whole image (`offset == RVA`). Multiple RVAs = duplicate
copies of the same literal, all of which are xref'd below.

| key literal | string RVAs (VA) |
|---|---|
| `ktol_lobby_invite_data` | 0x141563DF0, 0x141563F00 |
| `ktol_lobby_unlock` | 0x141563E20, 0x141563E68, 0x141563EB8 |
| `ktol_lobby_owner_name` | 0x141563E38, 0x141563ED0, 0x1415643A8 |
| `ktol_lobby_lock_status` | 0x141563E50, 0x141563E90, 0x141563EE8, 0x141564378 |
| `ktol_lobby_lock` | 0x141563E80, 0x141564398 |
| `ktol_empty_data` | 0x141567090 |
| `ktol_invite_data` | 0x141586A50 |
| `ktol_owner_id` | 0x141586CC0 |
| `ktol_owner_name` | 0x141586CD0 |
| `ktol_owner_ping_location` | 0x1415871B0 |
| `ktol_session_lock` | 0x141587230 |
| `ktol_lock_status` | 0x141587248 |
| `ktol_session_unlock` | 0x141587260 |
| `ktol_common_key` | 0x141587870 |

Two traps worth knowing before reading any xref list:

* `ktol_lobby_lock_status` **contains** `ktol_lobby_lock`: an xref to
  0x141563E50 / 0x141563E90 / 0x141563EE8 / 0x141564378 is *lock_status*, not
  *lock*. Only 0x141563E80 and 0x141564398 are `ktol_lobby_lock` (next byte 0x00).
* The strings are laid out as an interleaved table with the pointers that follow
  them, e.g. at 0x1586CE0: `…,"ktol_owner_id","ktol_owner_name", 0x1415DC778,
  0x1405F1570, 0x1405F15C0, …` where `0x1405F1570/0x1405F15C0` are exactly
  `CSessionP2pInfo`'s vtable methods (`classmap.txt:` `CSessionP2pInfo  vtable
  0x141586CE8  methods: 0x1405f1570 0x1405f15c0`) — the neighbouring data is a
  vtable, not a xref to the string.

## 2. Two distinct families (this is the single most useful split)

| | Steam family | EOS family |
|---|---|---|
| names | `ktol_owner_id/_name/_ping_location`, `ktol_invite_data`, `ktol_common_key`, `ktol_lock_status`, `ktol_session_lock/_unlock` | `ktol_lobby_lock_status`, `ktol_lobby_lock/_unlock`, `ktol_lobby_invite_data`, `ktol_lobby_owner_name` |
| transport | `SteamAPI_ISteamMatchmaking_SetLobbyData/GetLobbyData` (vtable slots 20/19) | `EOS_LobbyModification_AddAttribute` / `EOS_LobbyDetails_CopyAttributeByKey` / `EOS_LobbySearch_SetParameter` |
| string home | 0x1586A50 .. 0x1587870 | 0x1563E20 .. 0x1563F00, 0x1564378 |
| code home | 0x1405E9xxx .. 0x14060Axxx | 0x1405C0Fxx .. 0x1405E42xx |
| live-log proof | `SetLobbyData key="ktol_lock_status" value="ktol_session_unlock"` | — |

`ktol_lock_status` is a **string-valued** key: its value is one of
`"ktol_session_lock"` / `"ktol_session_unlock"`. `ktol_lobby_lock_status` is the
EOS mirror: value `"ktol_lobby_lock"` / `"ktol_lobby_unlock"`.
The attribute-value type field passed to EOS is `3` (`EOS_LAT_STRING`-position),
and both EOS writers and the EOS search parameter use that same string encoding.

## 3. Per-key table

Function RVAs are `.pdata` ranges. "class" is from `analysis/classmap.txt` where
the function is a vtable method; `—` means it is not a vtable method (called
directly or through the layers below).

### Steam family

| key | string VA | referencing functions | what it does with the key |
|---|---|---|---|
| `ktol_owner_id` | 0x141586CC0 | **set** `0x5FA3E0` (`CSessionP2pGrantOwnerHandle`), `0x5FA920`, `0x604F20`<br>**get** `0x5E9D20`, `0x5F2010`, `0x5F4050` | publish my SteamID64 as `%llu` text; read back + parse (base 10) as an identity check |
| `ktol_owner_name` | 0x141586CD0 | **set** `0x5FA3E0` (`CSessionP2pGrantOwnerHandle`), `0x604F20`, `0x5FA920`<br>**get** `0x5E9D20`, `0x5F4050`, `0x6057B0` | publish `SteamFriends::GetPersonaName()`; read back, must be non-empty |
| `ktol_owner_ping_location` | 0x1415871B0 | **set** `0x604F20`, `0x5FA3E0`, `0x5FA920`<br>**get** `0x60A3B0` | publish the Steam datacenter ping map (`ord=2+0/3+0,…` in the live log); read in the search-result callback to rank candidate sessions |
| `ktol_invite_data` | 0x141586A50 | **set** `0x604F20`<br>**get** `0x5F0310` | 64-byte blob (all-zero while hosting in the live log); the reader decodes it via `0x1406002D0` |
| `ktol_common_key` | 0x141587870 | **set** `0x604F20`<br>**get** `0x603C80` | session key passed into the member snapshot (`0x1405F5A10`) |
| `ktol_lock_status` | 0x141587248 | **set** `0x5FB560`/`0x5FB810` (`CSessionP2pLockHandle`), `0x604F20`<br>**filter** `0x609EA0` (`CSessionSearchHandle`) | carries lock state; also the key of the matchmaking string filter. **Never read back via `GetLobbyData` (see §5)** |
| `ktol_session_lock` | 0x141587230 | `0x5FB560`, `0x5FB810` | the value written when locking |
| `ktol_session_unlock` | 0x141587260 | `0x5FB560`, `0x5FB810`, `0x604F20`, `0x609EA0` | the value written when unlocked, and the value the search filter matches |
| `ktol_empty_data` | 0x141567090 | `0x616E08` | compared (`strncmp`, n=16) against an attribute name whose type field is `0x7`; generic marshalling check, no lobby logic |

### EOS family

| key | string VA | referencing functions | what it does with the key |
|---|---|---|---|
| `ktol_lobby_lock_status` | 0x141563E50 | `0x5DB938` | EOS search parameter (filter) |
| | 0x141563E90 | `0x5D4DCC`, `0x5D796D`, `0x5D88CC`, `0x5D944C`, `0x5D9EAC` (5 identical 0x13D-byte clones), `0x5DC6D3` | key of the lock attribute written by `EOS_LobbyModification_AddAttribute` then `EOS_Lobby_UpdateLobby` |
| | 0x141563EE8 | `0x5DE770` | same, in the "owner name + unlock" publisher |
| | 0x141564378 | `0x5E1749`, `0x5E2CC0`(+`0x5E2D46`) | read back with `EOS_LobbyDetails_CopyAttributeByKey`, compared to `ktol_lobby_lock` |
| `ktol_lobby_lock` | 0x141563E80 | `0x5D4DCC`, `0x5D796D`, `0x5D88CC`, `0x5D944C`, `0x5D9EAC`, `0x5DC6D3` | the locked token written as the attribute value |
| | 0x141564398 | `0x5E1749`, `0x5E2D46` | the locked token compared against the read-back value |
| `ktol_lobby_unlock` | 0x141563E20 | `0x5DB938` | the value the EOS search filter matches |
| | 0x141563E68 | `0x5D4DCC`, `0x5D796D`, `0x5D88CC`, `0x5D944C`, `0x5D9EAC`, `0x5DC6D3` | the unlocked token written as the attribute value |
| | 0x141563EB8 | `0x5DE770` | the unlocked token written in the publisher |
| `ktol_lobby_invite_data` | 0x141563DF0 | `0x5C0F90`, `0x5C5F10`, `0x5C8760`, `0x5CA350`, `0x5CBAD0` (5 identical 0x10D-byte clones), `0x5D5930` | `EOS_LobbyDetails_CopyAttributeByKey` — part of the session-info blob handed to the caller |
| | 0x141563F00 | `0x5DE8E4` | `EOS_LobbyModification_AddAttribute` — publisher of the invite blob |
| `ktol_lobby_owner_name` *(bonus, same table)* | 0x141563E38 | `0x5DC0D9` | literal stored into each search-result candidate record |
| | 0x141563ED0 | `0x5DE770` | written as an attribute |
| | 0x1415643A8 | `0x5E3FBA`, `0x5E421F` | written as an attribute (two more publishers) |

### 3.1 Verbatim excerpts (≤6 lines)

`0x5FB6A1` — `CSessionP2pLockHandle`, emits the Steam lock state
(`0x5FB560`, VA 0x1405FB6A1..0x1405FB6B7):

```
0x1405fb6a1  cmpb   $0x0,0x11fe198(%rip)        # 0x1417f9840     ; global lock flag
0x1405fb6a8  cmovne %rdx,%r9                                    ; r9 = locked? "ktol_session_lock" : "ktol_session_unlock"
0x1405fb6ac  lea    0xf8bb95(%rip),%r8          # 0x141587248 KEY ktol_lock_status
0x1405fb6b3  mov    0x8(%rbx),%rdx                              ; lobby id
0x1405fb6b7  call   *0xa0(%rax)                                 ; ISteamMatchmaking slot 20 = SetLobbyData
```

`0x5DBBE0` — the EOS search lock filter (`0x5DB938`, VA 0x1405DBBD6..0x1405DBC27):

```
0x1405dbbd6  cmpb   $0x0,0x1c(%rbx)                        ; [session+0x1c] == 0 ?
0x1405dbbe0  lea    0xf88269(%rip),%rax  # 0x141563e50 KEY ktol_lobby_lock_status
0x1405dbbf7  lea    0xf88222(%rip),%rax  # 0x141563e20 KEY ktol_lobby_unlock
0x1405dbc18  movq   $0x3,-0x18(%rbp)                       ; value-type slot = 3 (string form)
0x1405dbc1f  movq   $0x0,-0x78(%rbp)                       ; comparison-op slot = 0 (equal)
0x1405dbc27  call   *0xbbf443(%rip)     # 0x14119b070 EOS_LobbySearch_SetParameter
```

`0x5E1883` — the EOS lock predicate (`0x5E1749`, VA 0x1405E1883..0x1405E18C0):

```
0x1405e1883  call   *0xbb9a6f(%rip)     # 0x14119b2f8 EOS_LobbyDetails_CopyAttributeByKey
0x1405e189a  mov    0x10(%rdx),%rdx                        ; Attribute.Value.AsUtf8
0x1405e189e  lea    0xf82af3(%rip),%rcx  # 0x141564398 KEY ktol_lobby_lock
0x1405e18a5  mov    $0xf,%r8d                              ; 15 == strlen("ktol_lobby_lock")
0x1405e18ab  call   0x141127550                            ; strncmp(a,b,n), 0 == equal
0x1405e18b6  cmove  %r14d,%ecx                             ; equal -> locked = 1
0x1405e18ba  cmp    %cl,0xa0(%rbx)                         ; [session+0xa0] = cached locked flag
```

`0x60A14E` — the Steam matchmaking filter (`0x609EA0`, VA 0x14060A132..0x14060A15F):

```
0x14060a132  cmpb   $0x0,0x590(%rbx)                       ; [session+0x590] == 0 ?
0x14060a14e  xor    %r9d,%r9d                              ; comparison = k_ELobbyComparisonEqual (0)
0x14060a151  lea    0xf7d108(%rip),%r8  # 0x141587260 KEY ktol_session_unlock
0x14060a158  lea    0xf7d0e9(%rip),%rdx  # 0x141587248 KEY ktol_lock_status
0x14060a15f  call   *0x28(%rax)                           ; slot 5 = AddRequestLobbyListStringFilter
```

`0x603CD4` — client-side member arrays sized by the lobby's advertised member
limit (`0x603C80`, called from `CSessionJoinHandle` `0x603860` state 0):

```
0x140603cd0  mov    0x20(%rsi),%rdx                         ; lobby id
0x140603cd4  call   *0x100(%rax)                            ; slot 32 = GetLobbyMemberLimit
0x140603cda  mov    %eax,%ebp
0x140603d0b  mov    %rax,0x1a0(%rbx)                        ; member array[limit]
0x140603d12  cmp    $0x2,%ebp
0x140603d15  jle    0x140603d26
0x140603d1f  mov    %rax,0x1a8(%rbx)                        ; second array[limit-2] when limit > 2
```

### 3.2 Steam vtable slot anchors used above

`ISteamMatchmaking` via `SteamInternal_ContextInit(&0x1417FBC58)`:

| slot | offset | meaning | anchor |
|---|---|---|---|
| 5 | 0x28 | `AddRequestLobbyListStringFilter(key,value,cmp)` | 0x14060A15F, called with `rdx`=key, `r8`=value, `r9d`=0 |
| 9 | 0x48 | `AddRequestLobbyListDistanceFilter` | 1 arg at 0x14060A1A7 |
| 10 | 0x50 | `AddRequestLobbyListResultCountFilter` | 1 arg at 0x14060A1C3 |
| 12 / 4 | 0x60 / 0x20 | `GetLobbyByIndex` / `RequestLobbyList` | 0x14060A4CB / 0x14060A1D9 |
| 19 | 0x98 | `GetLobbyData` | every `ktol_*` read is this call |
| 20 | 0xA0 | `SetLobbyData` | log line 161 + 0x1405FB6B7 |
| 32 | 0x100 | `GetLobbyMemberLimit` | **inferred** from slot ordering, anchored by 19/20/35 |
| 35 | 0x118 | `GetLobbyOwner` | log: "vtable offset 0x118 (slot 35)" |

## 4. Gate hypothesis (ranked)

Ranking = how directly the code decides "may this client proceed towards joining
this session", with the observed condition and constants.

**1. Steam matchmaking string filter on `ktol_lock_status` — `0x609EA0`
(helper called by `CSessionSearchHandle` vtable method `0x609D50`; vtable
0x141587D20), filter call at 0x14060A14E. VERIFIED.**
Before `RequestLobbyList` it adds `AddRequestLobbyListStringFilter("ktol_lock_status",
"ktol_session_unlock", 0)` unless the byte at `[session+0x590]` is non-zero.
A host whose lobby data says `ktol_session_lock` is therefore *absent from the
client's lobby list* — the gate is at discovery, not at join. Results are handled
by the CallResult `0x60A3B0` (registered at 0x14060A211), which per candidate does
`GetLobbyData(…,"ktol_owner_ping_location")` (0x14060A4E9), decodes the candidate's
blob (`0x1406002D0`), compares against an expected record (`cmp %ebx,(%rdi)` at
0x14060A487) and writes a 0x8E0-byte candidate record tagged `1`/`6`
(0x14060A615 / 0x14060A6AD). Nothing here re-checks the lock value.

**2. EOS search parameter on `ktol_lobby_lock_status` — `0x5DB938`, parameter call
at 0x1405DBC27. VERIFIED.**
`EOS_LobbySearch_SetParameter` with `Key="ktol_lobby_lock_status"`,
`Value.AsUtf8="ktol_lobby_unlock"`, `ComparisonOp=0` (equal, `movq $0x0,-0x78(%rbp)`),
`ValueType=3` (string), skipped when `[session+0x1c] != 0`; failure surfaces as
`0x80C82004`. Same design as #1: locked EOS lobbies are filtered out of the search
result set. The EOS join itself (`EOS_Lobby_JoinLobby` at six identical clone
sites, e.g. 0x1405D3FF2, 0x1405D7753, 0x1405D8742, 0x1405D92C2, 0x1405D9D52,
0x1405DCC8A) carries **no lock test of its own** — the surrounding code is the same
rotating-slot/GetTickCount boilerplate in all six.

**3. Lobby-data validation during join — `0x5F2010` (0x622 bytes). PARTIALLY
VERIFIED (the mechanism; the exact caller is unresolved).**
It walks an array of 0x20-byte expected records (`cmpl $0x1,-0x8(%rdi)` selects
type 1), builds each key with `snprintf("%s%d", "integer_", n)` — `0x1405DB3B0`
with format 0x1415863B0 `"%s%d"` and prefix 0x141586C08 `"integer_"`, the same
`integer_NN` vocabulary the live log shows — fetches it with `GetLobbyData`
(0x1405F2142), decodes the value (`0x1406002D0`), requires
`decoded_len == expected_len` (`cmp %ebx,(%rdi)`, 0x1405F2252) and
`memcmp(expected, decoded, len) == 0` (`0x1410FB9C0`, 0x1405F2264). Then it reads
`ktol_owner_id` (0x1405F22FE), parses it base-10 (`0x14112EF3C`, `r8d = 0xa`),
calls `GetLobbyOwner` (slot 0x118, 0x1405F235B) and compares the two ids
(`mov -0x79(%rbp),%rax; cmp %rax,-0x71(%rbp); jne …` at 0x1405F24A1).
So: "the lobby's advertised data must match what I expect, and its owner must be
who I think it is".

**4. Owner-identity check on a joined lobby — `0x5E9D20` and its twin `0x5F4050`.
VERIFIED mechanism / INFERRED intent.**
`GetLobbyData("ktol_owner_name")` (0x1405E9DBD) must be non-NULL and non-empty,
then `GetLobbyData("ktol_owner_id")` (0x1405E9E1C) likewise, then decimal parse
(`mov $0xa,%r8d; call 0x14112EF3C`) and a comparison with a SteamID; the failure
return is `0x80C82100`. `0x5E9D20` is called from `0x140D75C48` — the session-task
layer (`0xD75A..0xD760..`), not from the search path.

**5. EOS "is this session locked?" predicate — `0x5E1749` and the pair
`0x5E2CC0`+`0x5E2D46` (split by `.pdata` at 0x5E2D46, one logical routine).
VERIFIED.**
Condition: `strncmp(attr("ktol_lobby_lock_status").Value.AsUtf8, "ktol_lobby_lock",
15) == 0` with the compare helper at `0x141127550`. Result is cached at
`[session+0xa0]`; when the cached value changes, a change notification fires
(`0x5E1749` also re-reads the lobby owner via `EOS_LobbyDetails_GetLobbyOwner` and
applies it through `0x1405E3E20`). No consumer of `[session+0xa0]` was found by
call-site analysis (these are dispatch-reached, zero direct callers).

**6. The lock *writers* (not gates) — where "locked" comes from. VERIFIED.**
`0x5EAF60` is the state setter: it compares the requested byte with
`[session+0xb8]`, rejects no-ops with `0x80C82020`, and stores the new value into
the global byte `0x1417F9840` (`mov %dil,0x120e758(%rip) # 0x1417f9840` at
0x1405EB0E1). Its only two callers are `0x140D75666` (in `0x140D755F0`) and
`0x140D75FC9` (in `0x140D75F10`) — the session-task wrappers that reach it via
`0x1405EA110` (a "can lock" precondition). The EOS counterpart is `0x1405BFC60`/
`0x1405C0820` on `[session+0x60]` (15 direct callers in 0x140D71..0x140D75).
Downstream, `0x1417F9840` is read at `0x1405FB669`/`0x1405FB6A1`/`0x1405FB872`/
`0x1405FBA38` and written at `0x1405FBB39` inside `0x1405FBAC0` — the Steam
`LobbyDataUpdate` callback (`SteamAPI_RegisterCallback(this+0x48, 0x1f9, 0x1405FB9F0)`
at 0x1405FB6E0, 0x1F9 = 505 = `LobbyDataUpdate_t`).

**7. Invite blob — `ktol_invite_data` / `ktol_lobby_invite_data`. VERIFIED
mechanics, no comparison against a local secret found.**
Steam: published by `0x604F20` (0x1406051A3), read by `0x5F0310` (0x1405F0422) which
measures the string, allocates and decodes it via `0x1406002D0(value,len,buf,len)`;
a zero-length value yields `0x80C81014`. `0x5F0310`'s only caller is `0x1405ED2C0`
(the ktol session layer, the region of the mod's own backtrace frames). EOS:
published by `0x5DE8E4` (0x1405DE999), read by the five `0x5C0F90…0x5CBAD0` clones
and `0x5D5930`/`0x5D59FD` as part of a session-info copy. The live value while
hosting is 64 zero bytes, so it behaves as a *record slot* rather than a secret;
**no memcmp of the invite blob against local state was found outside item #3's
`integer_*` record loop.**

Not gates, but load-bearing neighbours: `0x603C80` (see §3.1) sizes the client's
member arrays from `GetLobbyMemberLimit` and branches on `limit > 2`;
`0x5F5A10` then allocates `count * 0x98` bytes of member records at
`[manager+0x130]` (`mov $0x98,%eax; mul %r13`, 0x1405F5A3F..0x1405F5AF9).

## 5. Explicit absences

* **No `GetLobbyData` of `ktol_lock_status` / `ktol_session_lock` /
  `ktol_session_unlock` exists anywhere in `.text`.** All 9 `GetLobbyData` sites
  that touch a `ktol_*` key read only `ktol_owner_name` (0x1405E9DC7, 0x1405F40E5,
  0x140605824), `ktol_owner_id` (0x1405E9E1C, 0x1405F22FE, 0x1405F413C),
  `ktol_invite_data` (0x1405F0422), `ktol_common_key` (0x140603D6E),
  `ktol_owner_ping_location` (0x14060A4F5). Consequence: the Steam client never
  *reads* the lock key — **the only Steam-side lock gate is the search filter (#1)**.
* **No 8-byte pointer anywhere in the 50,779,928-byte image is equal to any
  `ktol_*` string VA** (`tools/ktol_refs.py ptrs`, all 23 literal sites): the keys
  are reached exclusively by RIP-relative `lea`, i.e. the tables that use them are
  built at runtime, and a static pointer scan is not a completeness argument here.
* **No `ktol_*` reference in the P2P packet layer**: above 0x140600000 only two
  functions reference any key at all — the search CallResult `0x60A3B0` (inside the
  `CPacket*` address span) and `0x616E08` (`ktol_empty_data`). The `CPacket*` vtable
  methods themselves (0x1406088A0 `CPacketBye` … 0x14060D9B0 `CPacketLeave`) contain
  none. `CPacketJoinRequest`'s methods (`0x14060C3C0`, `0x14060C440`, `0x14060CB50`,
  0x14060CC10) contain no lock/owner check — they are constructor/serialise stubs
  (`memset(0x80)`, vtable store, ref-count bump). So there is **no host-side
  lock enforcement of the join request in this image**; a host that is locked
  simply does not appear in a compliant client's search results.
* `ktol_empty_data` has exactly one xref (0x140616E1D, `strncmp(…, 16)`), guarded by
  `cmpl $0x7,0x14(%rax)`; **no code xref found** connecting it to any lock/invite/join
  decision.
* `EOS_Lobby_JoinLobby` callers carry no key check (six byte-identical clones).
* No xref to `ktol_lobby_lock` (0x141563E80 / 0x141564398) exists outside the six
  writers and the two readers listed in §3.

## 6. Confidence

| Item | Confidence |
|---|---|
| Key string RVAs, duplicate sets, the lock/lock_status prefix trap | **verified** (byte scan + boundary check) |
| Function boundaries / every listed RVA as a `.pdata` function | **verified** |
| Import names (`EOS_Lobby*`, `SteamInternal_ContextInit`, …) | **verified** (import descriptors intact in the dump) |
| Steam family vs EOS family split, `ktol_lock_status` string value | **verified** (code + live log line 161) |
| `CSessionP2pLockHandle` (0x5FB560) / `CSessionP2pGrantOwnerHandle` (0x5FA3E0) / `CSessionJoinHandle` (0x603860) / `CSessionSearchHandle` (0x609D50) names | **verified** (`analysis/classmap.txt` vtables 0x1415872A8 / 0x141587208 / 0x1415878D0 / 0x141587D20) |
| Gate #1 Steam filter, gate #2 EOS filter, gate #5 strncmp predicate, gate #6 lock-flag chain | **verified** (verbatim excerpts above) |
| Gate #3 `0x5F2010` validation loop + owner-id compare | **inferred** — the mechanism is disassembled, but the function has no direct caller (dispatch-reached) and I did not resolve which task invokes it |
| Gate #4 owner-name/id check direction (`0x5E9D20`, `0x5F4050`) | **inferred** — the read/parse is verified; whether equality is "accept" or "reject" depends on the un-shown path at 0x1405E9E55 |
| Slot 32 = `GetLobbyMemberLimit` (0x603CD4) | **inferred** — SDK vtable ordering, anchored by slots 5/9/10/19/20/35 |
| `0x60A3B0` candidate scoring / `0x5E*` intent wording ("locked", "publisher") | **inferred** labels on verified instructions |
| `ktol_lobby_owner_name` at 0x5DC0E1 being stored (not compared) in the candidate record | **inferred** — `mov %rsi,-0x48(%rbp)` at 0x1405DC1F0 is visible; no `strncmp`/`CopyAttributeByKey` follows in that function |
| Invite-data blob semantics | **inferred** — only length + `0x80C81014` (empty) handling is visible in `0x5F0310`; the 64-zero-byte value is from the live log |
| EOS option-struct field roles (`$0x3` type slot, comparison-op slot 0, api-version `$0x1`) | **inferred** — read from the stack layout at the call sites; the *values* are verified, the field *names* are mapped from the EOS SDK's documented struct order |
