# RE: the session open/join path (Steam backend)

Offline static analysis of the runtime image. Read-only: no game launch, no build,
no test, no network. Every address below is a VA (`0x140000000` + RVA); the dump is
`~/.cache/nioh3p/dumps/nioh3p_image_merged.bin` (offset == RVA).

Reproduce any window with:

```
python3 tools/ktol_refs.py dis 0x1405f6b20 0x180        # annotated window
python3 tools/ktol_refs.py callers 0x1406047f0          # direct call sites
objdump -D -b binary -m i386:x86-64 --adjust-vma=0x140000000 \
  ~/.cache/nioh3p/dumps/nioh3p_image_merged.bin \
  --start-address=0x140603860 --stop-address=0x140603c80
```

## 0. Method and trust

| Fact used | Evidence |
|---|---|
| Function boundaries | `.pdata` `RUNTIME_FUNCTION` table (RVA 0x2EE1000), read via `tools/ktol_refs.py:functions()`. All ranges below are `[begin,end)`. Note: **small leaf thunks are not in `.pdata`** (`0x1406047e0`, `0x140603850`), so a vtable slot can point outside any `.pdata` range — verified |
| Import names | `IMAGE_IMPORT_DESCRIPTOR` array at RVA 0x1761EE0 is intact; only the IAT is patched. `steam_api64.dll` has 11 imports: slots `0x14119BC78..0x14119BCC8`, incl. `SteamAPI_RegisterCallback` @0x14119BCA8, `SteamAPI_RegisterCallResult` @0x14119BCB8, `SteamInternal_ContextInit` @0x14119BCC8 — verified |
| Class/vtable/method mapping | `analysis/classmap.txt` (RTTI) — verified against code (every vtable below is stored by a constructor) |
| Cross-checks | For each key method the method VA occurs **exactly once** in the whole 50,779,928-byte image, as a vtable slot (see §1). Nothing else takes its address |
| Markers | **verified** = seen in disassembly at the given RVA; **inferred** = a label/role derived from verified instructions and the anchored vtable-slot map |

The ISteamMatchmaking slot→name map used throughout (anchored by `ktol` doc §3.2:
slot 19 `GetLobbyData` @0x1405FB6B7, slot 20 `SetLobbyData` @0x14060A15F,
slot 35 `GetLobbyOwner` @0x1405F235B) is:

| slot | offset | name | used at |
|---|---|---|---|
| 1 | 0x08 | `BLoggedOn` (via `ISteamUser`, ctx 0x1417FBCB8) | 0x1405EECF2 — **inferred** |
| 4 | 0x20 | `RequestLobbyList` | 0x14060A1D9 (ktol doc) |
| 5 | 0x28 | `AddRequestLobbyListStringFilter` | 0x14060A15F (ktol doc) |
| 9/10/12 | — | distance/result-count/`GetLobbyByIndex` | ktol doc |
| 13 | 0x68 | `CreateLobby` | **0x140604CFA** — verified |
| 14 | 0x70 | `JoinLobby` | **0x1406038F2**, **0x140603F03** — verified |
| 15 | 0x78 | `LeaveLobby` | 0x1406012FF, 0x140603AA8, 0x140603B08, 0x140604204, 0x140604262 — verified |
| 19 | 0x98 | `GetLobbyData` | ktol doc |
| 20 | 0xA0 | `SetLobbyData` | ktol doc |
| 28 | 0xE0 | `RequestLobbyData` | **0x1405EF80E** — verified |
| 32 | 0x100 | `GetLobbyMemberLimit` | **0x140603CD4** — verified instruction, **inferred** name (ktol doc) |
| 35 | 0x118 | `GetLobbyOwner` | 0x1405EF90C, 0x1405F235B — verified |

Slot 17 (`GetNumLobbyMembers` under that ordering) is **not called anywhere in
the session block** — see §9.5. The two `call *0x88` sites in 0x1405E0000-
0x140610000 (0x1405ECFED, 0x1405F0205) test only `%al` (a bool) on a third
context global (0x1417FBCE8) and live in functions with no caller and no
vtable entry, so they are neither a member count nor on any live path.

Disambiguation: an indirect `call *0x20(%rax)` where `rax` is the
`CSessionP2pSteam` vtable (0x141586B00) is `CSessionP2pSteam::vfn4` (0x1405EE000,
"get the active open op"), **not** matchmaking slot 4 (see 0x1405EF2F1). Only
calls made on the pointer returned by `SteamInternal_ContextInit` are the
Steam interface slots.

The interface is fetched with `SteamInternal_ContextInit(&0x1417FBC58)`; the call
returns a pointer whose first qword is the `ISteamMatchmaking*`
(`mov (%rax),%rcx; mov (%rcx),%rax` at 0x140604CE3/0x140604CE6). The one-time
initialiser of that context global is the unnamed 0x1405EDC80:

```
0x1405edc89  call *0x14119bcc0            ; SteamAPI_GetHSteamUser
0x1405edc91  lea  0x1415869e8,%rdx        ; "SteamMatchMaking009"
0x1405edc98  call *0x14119bc80            ; SteamInternal_FindOrCreateUserInterface
0x1405edc9e  mov  %rax,(%rbx)             ; *ctx = ISteamMatchmaking*
```
This is the frame `0x5edc9e (unnamed)` of the live backtrace: it is inside
0x1405EDC80, called by `SteamInternal_ContextInit`.

## 1. Class → vtable → methods

`CSessionP2pPacketHandler` has **no RTTI entry and no vtable in
`analysis/classmap.txt`** — see §4.11. Everything else:

| class | vtable VA | vfn0 | vfn1 | vfn2 | vfn3 | vfn4 |
|---|---|---|---|---|---|---|
| `CSessionP2p` (base) | 0x141586B30 | 0x1405EDF00 | 0x1405F8BC0 | pure 0x1410FB804 | pure | pure |
| `CSessionP2pSteam` | 0x141586B00 | 0x1405EE980 | 0x1405EEBD0 | **0x1405EEC80** | **0x1405EF690** | 0x1405EE000 |
| `CSessionP2pHandle` | 0x141586AD0 | 0x1405EFAA0 | 0x1405EFAE0 | **0x1405EFB10** | 0x1405EFB50 | 0x1405EFC00 |
| `CSessionP2pOpen` | 0x141587020 | 0x1405F66D0 | 0x1405F6730 | **0x1405F6780** | 0x1405F4DA0 | — |
| `CSessionP2pOpenHandle` | 0x141587078 | 0x1405F6A50 | 0x1405F6B10 | **0x1405F6B20** | 0x1405EFB50 | 0x1405EFC00 |
| `CSessionCreate` | 0x141587928 | 0x140604590 | 0x1406045F0 | **0x1405EFA40** | 0x1405F4DA0 | — |
| `CSessionCreateHandle` | 0x1415879C0 | 0x1406046D0 | 0x1406047E0 | **0x1406047F0** | 0x1405EFB50 | 0x140604550 |
| `CSessionJoin` | 0x141587888 | 0x140603670 | 0x1406036D0 | 0x1405EFA40 | 0x1405F4DA0 | — |
| `CSessionJoinHandle` | 0x1415878D0 | 0x140603780 | 0x140603850 | **0x140603860** | 0x1405EFB50 | 0x1405EFC00 |
| `CSessionP2pOpenErrorHandle` | 0x141587048 | 0x1405F6E30 | 0x1405F6E80 | 0x1405F6E90 | 0x1405EFB50 | 0x1405EFC00 |
| `CSessionP2pOpenParam` | 0x141586EB0 | 0x1405F0E60 | pure | 0x1405F0D60 | pure | — |
| `CSessionP2pOpenCreateParam` | 0x141586D80 | 0x1405F0E00 | 0x1405F0EC0 | 0x1405F0FF0 | 0x1405F11C0 | — |
| `CSessionP2pOpenJoinParam` | 0x141586D58 | 0x1405F1330 | 0x1405F1380 | 0x1405F1390 | 0x1405F13F0 | — |
| `CSessionP2pData` (base of the params) | 0x141586D28 | 0x1405F0A60 | pure | — | — | — |

**Single-occurrence proof (verified by pointer scan of the whole image):**

| method | only occurrence |
|---|---|
| 0x1405F6780 `Open::vfn2` | 0x141587030 (`Open` vtable slot 2) |
| 0x1406047F0 `CreateHandle::vfn2` | 0x1415879D0 (`CreateHandle` vtable slot 2) |
| 0x140603860 `JoinHandle::vfn2` | 0x1415878E0 (`JoinHandle` vtable slot 2) |
| 0x1405F6B20 `OpenHandle::vfn2` | 0x141587088 |
| 0x1405EEC80 `Steam::vfn2` | 0x141586B10 |
| 0x1405EF690 `Steam::vfn3` | 0x141586B18 |
| 0x1405EFA40 (shared poll) | 0x141586F38, 0x141587760, 0x141587898, 0x141587938, 0x141587D08 |

Corollary, and the answer to the "scan for references to the vtable slot
addresses" part: **no instruction anywhere in `.text` refers to
0x141587030, 0x1415879D0 or any other slot address**, neither by `lea`/`mov`
RIP-relative form nor as an 8-byte data pointer. There are also **zero direct
`e8` call sites** to 0x1405F6780 or 0x1406047F0. All dispatch is
`mov (%obj),%rax; call *0x10(%rax)`. — verified

## 2. Static session objects (the operations are singletons)

`.data` at 0x1417F9600.. is a block of pre-built, statically-initialised
operation objects (dumped from the image; **verified**):

| VA | vtable | field +8 | role |
|---|---|---|---|
| 0x1417F9608 | 0x141586F00 `CSessionP2pClose` | 4 | static close op |
| 0x1417F9630 | 0x141587078 `CSessionP2pOpenHandle` | 0 | **the open operation** |
| 0x1417F9650 | 0x141587928 `CSessionCreate` | 6 | embedded create op (handle +0x20) |
| 0x1417F9678 | 0x141587888 `CSessionJoin` | 7 | embedded join op (handle +0x48) |
| 0x1417F96A0 | 0x141587048 `CSessionP2pOpenErrorHandle` | 0 | error face of the open op |
| 0x1417F96C0 | 0x141587020 `CSessionP2pOpen` | 3 | **the open "process"** |
| 0x1417F96E8 | 0x1415870A8 `CSessionP2pRejectHandle` | 0 | static reject handle |
| 0x1417F9708 | 0x1415870D8 `CSessionP2pReject` | 0xC | static reject op |

Also static (separate, used when Create/Join are driven outside the open handle):

| VA | vtable | set by |
|---|---|---|
| 0x1417FBA90 | 0x1415879C0 `CSessionCreateHandle` | 0x1406045F0 (see §4.6) |
| 0x1417FBA40 | 0x1415878D0 `CSessionJoinHandle` | 0x1406036D0 (see §4.7) |

`CSessionP2pOpenHandle::vfn2` reads exactly the fields this block initialises:
`+0x08` inner, `+0x10` param, `+0x18` state, `+0x20` create op, `+0x48` join op
(the handle is 0x70 bytes — the constructor's `operator delete` size at
0x1405F6A9A). — verified

## 3. The singleton and the two structs that decide everything

`0x142C96250` is a global pointer to the single `CSessionP2pSteam` instance.
Verified store: `mov %rbp,0x142C96250` at 0x1405EE268 in the factory
0x1405EE200 (allocates 0x2B8 bytes, calls the constructor 0x1405EE720 at
0x1405EE23C). Every "session" path below dereferences it.

`CSessionP2pSteam` layout (from the constructor 0x1405EE720 and uses):

| off | contents |
|---|---|
| +0x000 | vtable 0x141586B00 |
| +0x008 | bit flags (bit1 "ready/enabled", bit2 "open pending", bit4/bit10 latches, bit30/bit31 error latches) |
| +0x168 | embedded `CCallback<CSessionP2pPacketHandler,LobbyChatMsg_t>`, id **507** |
| +0x190 | session context pointer (non-null after the first open) |
| +0x198 | member count (int) |
| +0x1A0 | member array, stride 0xA8 |
| +0x1A8 | second member array (only when limit > 2) |
| +0x1B0 | local member index (int) |
| +0x1B4 | "current/self" member index used by the callbacks |
| +0x240 | **embedded `CSessionP2pHandle`** (vtable stored at 0x1405EE81B) |
| +0x248 | inner session pointer of that handle (0 when idle) |
| +0x250 | tick base for the invite error grace |
| +0x254 | invite state: 0 idle, 2 owner==self, 3 owner!=self |
| +0x258 | owner SteamID recorded by the data-update callback |
| +0x260 | lobby id requested via `RequestLobbyData` (32-bit field, written by the invite handler) |
| +0x268 | embedded `CCallback<CSessionP2pSteam,GameLobbyJoinRequested_t>`, id **333** |
| +0x290 | embedded `CCallback<CSessionP2pSteam,LobbyDataUpdate_t>`, id **505** |

`ctx = [CSessionP2pSteam+0x190]` (allocated by the first open):

| off | contents |
|---|---|
| +0x010 | **request param object** (`CSessionP2pOpenCreateParam` or `OpenJoinParam`); `+0x08` = mode, `+0x20` = max-members (create) / lobby SteamID (join) |
| +0x018 | runtime: `+0x08` lobby SteamID, `+0x4D0/4D8` create member shadow, `+0x4DC/4E0` tick/timeout pair, `+0x4E4` join result latch, `+0x4EA` pending flag |

All offsets **verified** from the instructions cited in §4.

## 4. Function summaries

### 4.1 `CSessionP2pSteam::vfn2` — 0x1405EEC80 (`0x1405EEC80-0x1405EF68B`), the per-frame pump

Calls the embedded handle's vfn2, then `SteamAPI_RunCallbacks`, then evaluates
the backend flags and the invite state. Excerpt (verified):

```
0x1405eeca3  add   $0x240,%rcx
0x1405eecad  call  *0x10(%rax)          ; CSessionP2pHandle::vfn2  (→ Open → Create/Join)
0x1405eecb0  call  *0x14119bc98         ; SteamAPI_RunCallbacks    (all callbacks dispatch here)
0x1405eecb6  mov   0x8(%rbx),%ecx       ; flags
0x1405eef93  mov   %r14d,%ecx
0x1405eef96  sub   0xc4(%rax),%ecx      ; [ctx+0x18]+0xc4 — "how long since?"
0x1405eef9c  cmp   $0x4e20,%ecx         ; 20000 ms
```
Then, at 0x1405EEFE7 the invite state is consumed:

```
0x1405eefe7  mov   0x254(%rbx),%eax
0x1405eefed  test  %eax,%eax
0x1405eefef  je    0x1405ef061           ; idle
0x1405eeff1  cmp   $0x2,%eax
0x1405eeff4  jne   0x1405ef001
0x1405eeff6  mov   %edi,0x254(%rbx)      ; 2 → clear
0x1405ef001  cmp   $0x3,%eax
0x1405ef03b  movl  $0x80c81010,0x48(%rbp) ; 3 → fail (je 0x1405ef01a)
```
Tail (0x1405EF2D7-0x1405EF367) clears the per-state latches (bit4 at
0x1405EF308, bit10 at 0x1405EF34E) only when `manager->vfn4()` returns null,
i.e. when no open op is attached. — verified

### 4.2 `CSessionP2pSteam::vfn3` — 0x1405EF690 (`0x1405EF690-0x1405EF7A3`)

A no-op unless the flags say "open pending and no error"; then it reads
`[ctx+0x18]+0x4EA` and clears it, emitting trace event 7:

```
0x1405ef69f  mov  0x8(%rcx),%edx
0x1405ef6a7  test $0x1,%al            ; bit2 (0x4) must be set
0x1405ef6f3  cmpb $0x0,0x4ea(%rax)    ; pending byte
0x1405ef70b  lea  0x7(%rax),%ecx      ; trace id 7
0x1405ef70e  call 0x1405ed9b0
0x1405ef72f  movb $0x0,0x4ea(%rax)
```
No writer of `[ctx+0x18]+0x4EA` exists in 0x1405E0000-0x140610000 (byte scan:
only 0x1405EF6F5 read and 0x1405EF731 write). — verified

### 4.3 `CSessionP2pOpen::vfn2` — 0x1405F6780 (`0x1405F6780-0x1405F69A5`)

Polls the handle it points at (this+0x18) and turns its state into the caller's
result. Full logic; excerpt:

```
0x1405f67ab  mov   0x18(%rcx),%rcx
0x1405f67b7  call  *0x10(%rax)        ; CSessionP2pOpenHandle::vfn2
0x1405f67ba  mov   0xc(%rdi),%eax     ; this->state
0x1405f67bd  test  %eax,%eax
0x1405f67bf  je    0x1405f67ea        ; 0 → still running, return true
0x1405f67c4  jne   0x1405f6809        ; (cmp $2) 1 → error bookkeeping
0x1405f67c6  mov   %ebx,0xc(%rdi)     ; 2 → reset state
0x1405f67df  lea   0x1417f96a0,%rax   ; this+0x18 = static OpenErrorHandle
```
State 1 additionally calls the trace helper 0x1405ED9B0 with category 3 and
sets `[CSessionP2pSteam+8] |= 4` (0x1405F689A) or `&= 0x7fffffff`
(0x1405F68B8). — verified

### 4.4 `CSessionP2pOpenHandle::vfn2` — 0x1405F6B20 (`0x1405F6B20-0x1405F6D6A`)

The mode selector and the state machine:

```
0x1405f6b41  mov  0x8(%rcx),%rcx
0x1405f6b4f  call *0x10(%rax)          ; poll inner Create/Join op if attached
0x1405f6bc8  mov  0x142c96250,%rax
0x1405f6bcf  mov  0x190(%rax),%rcx      ; ctx
0x1405f6bda  cmpl $0x0,0x8(%rax)        ; [ctx+0x10]+8 = mode
0x1405f6be0  movl $0x1,0x18(%rbx)       ; mode 0 → state 1 → CREATE op (+0x20)
0x1405f6c03  cmpl $0x1,0x8(%rax)
0x1405f6c09  movl $0x2,0x18(%rbx)       ; mode 1 → state 2 → JOIN op (+0x48)
```
On completion it writes the result into the shared param
(`param+0xc = 2`, `param+0x10 = error`, `param+0x20 = final state`) at
0x1405F6B79/0x1405F6C59 and calls its own vfn1 to reset (0x1405F6CAC).
State 0 additionally arms the op (0x1405F6C5F-0x1405F6CA3): `param+0xc = 1`,
then the **request's** vfn1 (0x1405F6CA3 `call *0x8(%rax)`, i.e. 0x1405F0EC0 for
create / 0x1405F1380 for join) and `this->vfn1()` (0x1405F6CAC). — verified

### 4.5 `CSessionP2pOpenHandle::vfn3` (0x1405EFB50) and vfn1 (0x1405F6B10)

`vfn3` = "attach inner op"; used by the open builder:

```
0x1405efb68  test %r8,%r8
0x1405efb6f  movl $0x80c80002,(%rdx)    ; null param
0x1405efb8b  cmpq $0x0,0x8(%rcx)
0x1405efb94  movl $0x80c81003,(%rdx)    ; already attached
0x1405efbbb  call *0x8(%rax)            ; param->vfn1 (builds the inner op)
0x1405efbe1  mov  %rdi,0x8(%rsi)        ; this->inner = param
```
`vfn1` (0x1405F6B10) clears `+0x08`, `+0x10`, `+0x18`. — verified

### 4.6 `CSessionCreate::vfn1` (0x1406045F0) and `CSessionCreateHandle::vfn2` (0x1406047F0)

`CSessionCreate::vfn1` points the process at the **static** create handle:

```
0x1406045fa  mov  (%rcx),%rax
0x140604603  call *0x18(%rax)            ; own vfn3 (clear)
0x140604608  mov  %rbx,0x1417fbaa0       ; g_create_proc = this
0x14060460f  lea  0x1417fba90,%rax       ; static CSessionCreateHandle
0x140604616  mov  %ecx,0x1417fbaa8
0x14060461c  mov  %rax,0x18(%rbx)        ; this->inner = &static handle
0x140604628  mov  %rcx,(%rdi)            ; *out = 0
```

`CSessionCreateHandle::vfn2` is a 3-state machine on `this+0x18`
(`0x140604839`), with the timer object `rdi = [ctx+0x18]`:

```
0x1406048c6  call 0x140604940            ; state 0 (0x1406048BE) → create worker
0x140604873  call *0x14119b828           ; state 1 → GetTickCount
0x140604879  sub  0x4dc(%rdi),%eax
0x14060487f  cmp  0x4e0(%rdi),%eax       ; timeout?
0x140604887  movl $0x2,0x18(%rbx)
0x14060488e  movl $0x80c82001,0x40(%rsp) ; 0x80C82001
0x14060485a  inc  %ecx                   ; state 2 (0x14060484E)
0x14060485c  mov  %ecx,0xc(%rax)         ; param->state = 1|2
```
The create worker 0x140604940 (`0x140604940-0x140604F14`, unnamed, called only
from 0x1406048C6) sizes the member arrays from the request's max-members
(`[param+0x20]`, 0x1406049DD; `> 2` branch at 0x1406049F7-0x140604A25), builds
the ping string, snapshots the member list (0x1405F5A10 at 0x140604AD5), and
then:

```
0x140604cdd  call *0x14119bcc8            ; SteamInternal_ContextInit
0x140604ce3  mov  (%rax),%rcx             ; ISteamMatchmaking*
0x140604ce9  mov  $0x2,%edx               ; public
0x140604cee  cmpb $0x0,0x25(%r15)         ; "private" flag
0x140604cf3  cmovne %ebx,%edx             ; → 0
0x140604cf6  mov  0x20(%r15),%r8d         ; max members
0x140604cfa  call *0x68(%rax)             ; slot 13 = CreateLobby
0x140604d32  call *0x14119bcb8            ; RegisterCallResult(0x140604f20)
```
0x140604F20 is the `LobbyCreated_t` CallResult and the only publisher of the
`ktol_*` Steam keys: `ktol_owner_ping_location` 0x140605052,
`ktol_owner_name` 0x1406050A1, `ktol_owner_id` 0x140605100,
`ktol_invite_data` **0x1406051A3**, `integer_` 0x140605286, `binary_` 0x140605318,
`ktol_common_key` 0x140605515, `ktol_session_unlock` 0x140605539,
`ktol_lock_status` 0x140605540. Timer 15000 ms (0x3A98) at 0x140604D46.
— verified

### 4.7 `CSessionJoinHandle::vfn2` — 0x140603860 (`0x140603860-0x140603C80`)

Six-state switch on `this+0x18` (jump table at 0x140603897, table 0x140603C68):

| state | RVA | action |
|---|---|---|
| 0 | 0x1406038A3 | call 0x140603C80 (join prep: `GetLobbyMemberLimit`, member snapshot) → state 1 |
| 1 | 0x1406038D3 | **JoinLobby** + `RegisterCallResult(LobbyEnter)` + 15000 ms → state 2 |
| 2 | 0x140603984 | timeout → error 0x80C82011 → state 5 |
| 3 | 0x1406039F2 | call 0x14060CCC0 (packet-layer join request) → 30000 ms → state 4 |
| 4 | 0x140603A4B | timeout → `LeaveLobby` (0x140603AA8) → state 5 |
| 5 | 0x140603AD3 | read the result latch `[ctx+0x4E4]`, finish |

The join itself, verbatim:

```
0x1406038d3  call 0x1405effc0            ; request, only if mode == 1
0x1406038d8  mov  0x20(%rax),%rbx        ; target lobby SteamID
0x1406038dc  lea  0x1417fbc58,%rcx
0x1406038e3  call *0x14119bcc8           ; SteamInternal_ContextInit
0x1406038e9  mov  (%rax),%rcx
0x1406038f2  call *0x70(%rax)            ; slot 14 = JoinLobby(lobby)
0x14060392a  call *0x14119bcb8           ; RegisterCallResult(handler 0x140604310)
```
0x140604310 is the `LobbyEnter_t` CallResult:

```
0x140604337  mov  0x10(%rdx),%eax        ; m_EChatRoomEnterResponse
0x14060433a  cmp  $0x1,%eax
0x14060433d  je   0x140604392           ; OK → save lobby id, state 3
0x14060433f  cmp  $0x4,%eax
0x140604344  movl $0x80c82010,0x40(%rsp) ; response 4 → error
0x14060436b  movl $0x80c82002,0x40(%rsp) ; anything else → error
```
The second, standalone join helper 0x140603EC0 (`0x140603EC0-0x140603FD2`)
does the same thing without the state machine (0x1405EFFC0 → `JoinLobby`
0x140603F03 → CallResult + 15000 ms). It has **no direct caller** and no vtable
entry in `analysis/classmap.txt`. — verified

### 4.8 `CSessionP2pHandle::vfn2` — 0x1405EFB10

```
0x1405efb19  mov  0x8(%rcx),%rcx
0x1405efb25  call *0x10(%rax)        ; active op (Open)::vfn2
0x1405efb28  test %al,%al
0x1405efb2a  jne  0x1405efb3b        ; still running
0x1405efb2c  mov  (%rbx),%rax
0x1405efb37  jmp  *0x8(%rax)         ; own vfn1 → drop the op
```
vfn3 = 0x1405EFB50 (attach, §4.5), vfn4 = 0x1405EFC00
(`mov 0x8(%rcx),%rax; ret` = get active op). — verified

### 4.9 The Steam invite callbacks (registered in the constructor)

Constructor 0x1405EE720 registers three callbacks; the handler pointers are
stored as a member-function-pointer pair `{handler, 0}` at callback `+0x18`:

| callback | object offset | id | handler |
|---|---|---|---|
| `CCallback<CSessionP2pSteam,GameLobbyJoinRequested_t>` | +0x268 (reg 0x1405EE886) | 333 | **0x1405EF7E0** (0x1405EE851) |
| `CCallback<CSessionP2pSteam,LobbyDataUpdate_t>` | +0x290 (reg 0x1405EE8C7) | 505 | **0x1405EF820** (0x1405EE892) |
| `CCallback<CSessionP2pPacketHandler,LobbyChatMsg_t>` | +0x168 (reg 0x1405EE7EB) | 507 | **0x1405FC0D0** (0x1405EE7E0) |

**Invite handler 0x1405EF7E0** = "remember the invited lobby, refresh its data":

```
0x1405ef7e6  mov  (%rdx),%rax
0x1405ef7ec  mov  %rax,0x260(%rcx)      ; pending lobby = invite lobby
0x1405ef7f3  lea  0x1417fbc58,%rcx
0x1405ef7fa  call *0x14119bcc8           ; ContextInit
0x1405ef800  mov  (%rbx),%rdx           ; lobby id
0x1405ef806  mov  (%rcx),%rax
0x1405ef80e  jmp  *0xe0(%rax)           ; slot 28 RequestLobbyData
```
No other call, no state change. **verified**

**LobbyDataUpdate handler 0x1405EF820** = "is this my lobby, and is the owner me?":

```
0x1405ef8ca  mov  0x8(%rax),%rcx        ; [ctx+0x18]+8 = tracked lobby
0x1405ef8ce  cmp  (%rsi),%rcx           ; == update's lobby?
0x1405ef8d3  movl $0x3,0x254(%rdi)      ; no → state 3
0x1405ef8eb  cmpb $0x0,0x10(%rsi)       ; m_bSuccess
0x1405ef90c  call *0x118(%rax)          ; GetLobbyOwner
0x1405ef97a  cmp  0x58(%rsp),%rax       ; owner vs member[0x1B4].steamid
0x1405ef982  add   $0x2,%ebx            ; 2 = owner is me, 3 = not me
0x1405ef985  mov  %ebx,0x254(%rdi)
```
**verified**

### 4.10 The two entry points above all of this

**(a) 0x1405E9440 — `SessionOpen_Start` (unnamed), the only way to start an open.**
Preconditions, verbatim:

```
0x1405e94e2  mov  0x142c96250,%rax
0x1405e94e9  mov  0x8(%rax),%ecx
0x1405e94ec  shr  $0x1,%ecx
0x1405e94ee  test $0x1,%cl
0x1405e94f1  jne  0x1405e9520           ; bit1 set → continue
0x1405e9501  movl $0x80c81001,0x38(%rdx) ; else 0x80C81001
0x1405e9569  call *0x20(%rax)           ; manager->vfn4() = active op?
0x1405e956f  je   0x1405e9592           ; null → 0x1405e959a, else 0x80C80003
```
It also rejects bit30 (0x1405E952F-0x1405E9538) and bit31 (0x1405E954D-0x1405E9551)
and, on success, bumps the global op counter 0x142C96268 and stores it at
`[manager+0x1C]` and `*out` (0x1405E95E1-0x1405E95F3). — verified

**(b) 0x1405F71F0 — `SessionOpen_BuildAndAttach` (unnamed), the mode writer.**
The descriptor's first dword selects create vs join:

```
0x1405f7274  mov  (%r14),%eax            ; 0 = create, 1 = join
0x1405f7299  lea  0x141586d80,%rax       ; OpenCreateParam vtable
0x1405f72b4  mov  %r15d,0x8(%rbx)        ; mode 0
0x1405f731e  lea  0x141586d58,%rax       ; OpenJoinParam vtable
0x1405f7328  movl $0x1,0x8(%rbx)         ; **mode 1 = join**
0x1405f7334  mov  %rbx,0x10(%rbp)        ; ctx->request = p
0x1405f735b  call *0x10(%rax)            ; p->vfn2 (copy descriptor fields)
0x1405f756d  call *0x18(%rax)            ; handle vfn3 (attach 0x1417f96c0)
```
The mode/param it fills is read by the join op at 0x1406038D8 (`+0x20` = lobby
id) and by CreateLobby at 0x140604CF6 (`+0x20` = max members). The join param
fill is 0x1405F1390 (`OpenJoinParam::vfn2`: `*[desc+8]`→`+0x20`,
`[desc+0x14]`→`+0xC`, `[desc+0x18]`→`+0x10`, `+0x14 = 0x5DC`, `+0x18 = 3`);
create fill is 0x1405F0FF0 (`[desc+4]`→`+0x20`). — verified

### 4.11 The packet handler (`CSessionP2pPacketHandler`) — no RTTI entry

It is not in `analysis/classmap.txt` (no vtable, no `CSessionP2pPacketHandler`
RTTI). It only exists as the **owner object of a callback**: the constructor
embeds a 0x28-byte `CCallback<CSessionP2pPacketHandler,LobbyChatMsg_t>` at
`CSessionP2pSteam+0x168`, with vtable 0x141587368, callback id 0x1FB = 507,
owner pointer = `this+0x168`, and handler 0x1405FC0D0 (stored at
0x1405EE7E0). The handler reads a chat message:

```
0x1405fc13a  mov  0x8(%rsi),%rax          ; [ctx+0x18]+8 = lobby
0x1405fc13e  cmp  (%rbx),%rax             ; message's lobby
0x1405fc141  jne  0x1405fc29c             ; ignore foreign lobby
0x1405fc150  mov  0x142c96250,%rax
0x1405fc157  mov  0x8(%rax),%edx          ; flags
0x1405fc15f  test $0x1,%al                ; bit2 required
```
The two other `?$CCallback@VCSessionP2pPacketHandler…` entries in
`nioh3p.sites.txt` (0x5FCE40, 0x5FCFC0) are the callback's `Run`/destructor
slots, not class methods. — verified

## 5. Callers

### 5.1 Callers of `CSessionP2pOpen::vfn2` (0x1405F6780)

* **Direct `e8` call sites: none.** (verified; `tools/ktol_refs.py callers 0x1405f6780` is empty)
* **References to its vtable slot address 0x141587030: none** in `.text` or `.data` (verified).
* The only dispatch is `CSessionP2pHandle::vfn2` (0x1405EFB10), indirect at
  **0x1405EFB25** (`call *0x10(%rax)` with `rax = *(handle+8)`).

Upward chain, all call sites verified:

| frame | call site | callee |
|---|---|---|
| 0x1405BE350 (list walker) | 0x1405BE371 (after `call *%rax`) | the function pointer at `[rbx+0x18]` |
| 0x1405ED440 (`0x1405ED440-0x1405ED9A1`) | **0x1405ED4CC** (return 0x1405ED4CF = the logged `0x5ed4cf`) | `[0x142C96250]->vfn2` = `CSessionP2pSteam::vfn2` |
| `CSessionP2pSteam::vfn2` | **0x1405EECAD** | `*(this+0x240)->vfn2` = `CSessionP2pHandle::vfn2` |
| `CSessionP2pHandle::vfn2` | **0x1405EFB25** | `Open::vfn2` |
| `CSessionP2pOpen::vfn2` | **0x1405F67B7** | `*(this+0x18)->vfn2` = `OpenHandle::vfn2` |

### 5.2 Callers of `CSessionCreateHandle::vfn2` (0x1406047F0)

* **Direct `e8` call sites: none.**
* **References to slot address 0x1415879D0: none.**
* Only dispatch: `CSessionCreate::vfn2` (0x1405EFA40) at **0x1405EFA55**
  (`call *0x10(%rax)` with `rax = (this+0x18)->vtable`), and the same shared
  poll function is used by `CSessionCreateHandle` when driven from the static
  handle.
* Inside `CSessionCreateHandle::vfn2`, the create worker 0x140604940 is called
  once, at **0x1406048C6**.
* That worker calls `SteamInternal_ContextInit` at **0x140604CDD** (return
  address 0x140604CE3). This is the backtrace entry the mod logged as
  "`CSessionCreateHandle::vfn2+0x4f3`": 0x1406047F0+0x4F3 = 0x140604CE3, but
  `.pdata` puts 0x140604CE3 inside **0x140604940-0x140604F14**, not inside
  vfn2 (which ends at 0x14060493E). The name is a nearest-symbol artefact.
  Likewise "`CSessionP2pSteam::vfn3+0x170`" = 0x1405EF800 is inside
  **0x1405EF7E0-0x1405EF815**, the invite handler, not vfn3 (ends 0x1405EF7A3).

So the logged chain is the **host/create** path:

```
0x5be371 → 0x5ed4cf → Steam::vfn2 → P2pHandle::vfn2 → Open::vfn2
 → OpenHandle::vfn2 → Create::vfn2 → CreateHandle::vfn2 → 0x604940
 → ContextInit → 0x5edc80 → FindOrCreateUserInterface("SteamMatchMaking009")
```
(`0x5edc9e` is the return address inside 0x1405EDC80.) The `0x5ef800` entry in
that list belongs to the invite handler and is **not** on this poll path; it is
either a stale stack slot or a walker artefact. — verified chain + stated
caveat

### 5.3 Callers of the open entry points

| callee | call site | enclosing function |
|---|---|---|
| 0x1405F71F0 | **0x1405E959A** | 0x1405E9440 — the only caller |
| 0x1405E9440 | **0x140D724D8** | 0x140D71E00-0x140D72536 — host task (Steam branch) |
| 0x1405E9440 | **0x140D732DF** | 0x140D73240-0x140D7337C — join-from-search task |
| 0x1405E9440 | **0x140D733DD** | 0x140D73380-0x140D73419 — join-by-lobby task |
| 0x140D71E00 | **0x140D653A8** | 0x140D6538D (task step; picks host impl) |
| 0x140D73380 | **0x140D658A8**, **0x140D661E8** | two task steps (Steam branch) |
| 0x140D73480 | 0x140D658B6, 0x140D661F6 | same steps, EOS branch |

Backend switch: `0x140D68F10()==0` ⇒ Steam, `!=0` ⇒ EOS
(0x140D6588E-0x140D658AF and 0x140D73277-0x140D7328B). The host task passes a
descriptor whose kind dword is explicitly zeroed at 0x140D724A1
(`mov %eax,(%rsi)` with `eax=0`, `rsi = task+0xC`) ⇒ **create**; the join task
sets `movl $0x1,0x20(%rsp)` at 0x140D733D3 ⇒ **join**. — verified

## 6. Gate candidates (ranked)

### 1. Mode gate — create vs join. `0x1405F6BDA` / `0x1405F6C03`. VERIFIED.
`[[0x142C96250]+0x190]+0x10` is the request param; its `+0x08` must be `1` for
anything ever to reach `JoinLobby`. The request object is freshly allocated by
0x1405F71F0 (`operator new` 0x1405F88D0 at 0x1405F7284 / 0x1405F7309) and the
mode is stored there at 0x1405F72B4 (0) and 0x1405F7328 (1) only; an
instruction scan for `c7 /1` stores with displacement 8 across all 168
functions that reference 0x142C96250 found no other writer of that field.
The source is the descriptor's kind dword (`mov (%r14),%eax` at 0x1405F7274).
Consequence: **a join requires a caller that passes descriptor kind 1**; the
Steam invite callback is not such a caller.

### 2. "Backend ready / idle" gate — `0x1405E94E9`, `0x1405E9569`. VERIFIED.
* `[CSessionP2pSteam+8] & 2` must be set, otherwise **0x80C81001**
  (0x1405E9501). Bit 1 is also required by the pump itself
  (`shr $1; test $1` at 0x1405EECC6-0x1405EECCB).
* `[+8]` bit 30 and bit 31 must be clear (0x1405E952F, 0x1405E954D).
* `CSessionP2pSteam::vfn4` (0x1405EE000 → handle vfn4 0x1405EFC00) must return
  null, i.e. no open op may already be attached, otherwise **0x80C80003**
  (0x1405E9583). Note the same tick clears the "open pending" latches only when
  this returns null (0x1405EF2F1, 0x1405EF337).
No writer of bit 1 of `[+8]` was found in 0x1405E0000-0x140610000 (byte scan
for `or $2,disp32(%reg)` produced no session-block hit); the flag is set
outside the block. — **absence stated**

### 3. Capacity/sizing gates — service-driven on join, request-driven on create
Join prep sizes two member arrays from the lobby's advertised limit and takes a
different branch when `limit > 2`:

```
0x140603cd0  mov  0x20(%rsi),%rdx
0x140603cd4  call *0x100(%rax)          ; GetLobbyMemberLimit
0x140603d0b  mov  %rax,0x1a0(%rbx)      ; array[limit]
0x140603d12  cmp  $0x2,%ebp
0x140603d1f  mov  %rax,0x1a8(%rbx)      ; second array[limit-2]
```
This is a **capacity**, not a "full" test: nothing compares the current member
count against the limit before joining. For the 3-player work, this is the
Steam-side place where a limit > 2 is already supported.

The host does **not** call `GetLobbyMemberLimit` at `0x1406049F7`.
That address is `cmp $2,%r14d`; the limit was loaded from `request+0x20`
at `0x1406049DD`. Both host arrays are allocated before `CreateLobby`.
Raising only the service limit left native host capacity 2 in a live run,
with no extra array. The `CSessionP2pOpenCreateParam::SetParam` hook now
raises the request before allocation; live reads then showed capacity 3
and a non-null extra-member array.


### 4. No member-count gate — `GetNumLobbyMembers` (slot 17) is never called. ABSENCE.
The only two `call *0x88` sites in the session block (0x1405ECFED, 0x1405F0205)
use a third context global (0x1417FBCE8) and test the result as a **bool**
(`test %al,%al` at 0x1405ECFF3 / 0x1405F020B); on false they return
**0x80C8A601** (0x1405ED03E, 0x1405F024F), on true they touch the friends
interface (ctx 0x1417FBCA0, slot 0x108). Both enclosing functions
(0x1405ECED0, 0x1405F0190) have **zero callers** and no vtable entry in
`analysis/classmap.txt`, i.e. dead code. Nothing compares a member count
against a limit before joining.

### 5. Invite-lobby owner gate — `0x1405EF8CE` … `0x1405EF985` → `0x1405EEFE7` … `0x1405EF03B`. VERIFIED.
The only check that compares a *lobby's* state with *local* state on the Steam
path: after `RequestLobbyData`, the `LobbyDataUpdate_t` handler sets
`+0x254 = 2` if the lobby owner equals the local member record, else `3`. The
pump then turns state 3 into **0x80C81010** immediately (0x1405EF004 →
0x1405EF01A → 0x1405EF03B) and state 1 into the same error after 5000 ms
(0x1405EF00D). State 2 is merely cleared (0x1405EEFF6).
**For an invited client the owner is the host, i.e. not self, so the invite
path always ends in 0x80C81010 — and never in `JoinLobby`.**

### 6. Lock gate — discovery only. `0x14060A151`/`0x14060A158`. VERIFIED (see `docs/RE_KTOL_KEYS.md` §4.1).
`CSessionSearchHandle` adds `AddRequestLobbyListStringFilter("ktol_lock_status",
"ktol_session_unlock", 0)` before `RequestLobbyList`. The join path performs
**no** lock test: there is no `GetLobbyData` of `ktol_lock_status`,
`ktol_session_lock` or `ktol_session_unlock` anywhere in `.text`, and the only
`JoinLobby` sites (0x1406038F2, 0x140603F03) have no key reference.
A locked host disappears from a compliant client's list, but a client that
reaches a join by another route (e.g. an invite) would not be stopped by it.

### 7. Invite blob — `ktol_invite_data`. `0x1405F0418` (read), `0x1406051A3` (write). VERIFIED.
Only two xrefs in the entire image (RIP-relative `lea`, reproduced with
`tools/ktol_refs.py xrefs ktol_invite_data`):
* **0x1405F0418** in 0x1405F0310-0x1405F0635: measures the string, allocates,
  decodes with 0x1406002D0; empty → 0x80C81014 (0x1405F03F6). Its only caller
  is 0x1405ED362 in 0x1405ED2C0 (the ktol session layer), **not** the open/join
  path.
* **0x1406051A3** in the `LobbyCreated` CallResult 0x140604F20 (publish).
No comparison of the invite blob against local state exists in the open/join
path. **The invite payload is not a join gate.**

### 8. LobbyEnter response gate — `0x140604337`. VERIFIED.
`m_EChatRoomEnterResponse`: 1 = admitted → state 3 (0x140604392, then
0x14060CCC0 at state 3); 4 → **0x80C82010**; anything else → **0x80C82002**
(0x140604344 / 0x14060436B). This is the only place Steam can tell the client
"that lobby is not joinable for you"; it happens **after** `JoinLobby`, so it
cannot explain a missing `JoinLobby`.

### Which check the client fails

If the host advertises a lobby that is not joinable for the client, the
Steam-side code has **no pre-join check at all** — no "full", no "locked", no
"joinable" boolean is read before `JoinLobby`. The three possible outcomes are:
`CreateLobby`/`JoinLobby` never called (gate 1/2 — mode and backend state),
or `JoinLobby` called and rejected via `LobbyEnter` response (gate 8), or — for
the invite route specifically — gate 5, which fires *before* any join and
produces 0x80C81010.

## 7. Why an invite can end with no `JoinLobby`

Verified end-to-end: for the Steam backend there are exactly **two**
`JoinLobby` call sites in the whole image's session block
(0x1405E0000-0x140610000, which contains every `CSessionP2p*` and `CPacket*`
class): **0x1406038F2** (`CSessionJoinHandle::vfn2`, state 1) and
**0x140603F03** (the caller-less helper 0x140603EC0). Both demand
`[ctx+0x10]+8 == 1` via 0x1405EFFC0 (0x1406038D3, 0x140603EE4). That request
object is only created by 0x1405F71F0, which is only reachable from
0x1405E9440, which is only called by the three task functions in §5.3.

The invite path is:

```
SteamAPI_RunCallbacks (0x1405EECB0)
  → CCallback<CSessionP2pSteam,GameLobbyJoinRequested_t>::Run (0x1405F07D0)
  → 0x1405EF7E0   : this->0x260 = lobby; RequestLobbyData(lobby)   [0x1405EF80E]
  → 0x1405EF820   : lobby match? owner == self? → this->0x254 = 2 or 3
  → 0x1405EEC80   : 0x254 == 3 → error 0x80C81010                  [0x1405EF03B]
```

There is **no edge from any of those three functions to 0x1405E9440**, and the
invite handler never touches `[ctx+0x10]`, `[ctx+0x10]+8`, or the task state.
The invite therefore cannot produce `JoinLobby`; at best it produces
`RequestLobbyData` + `GetLobbyOwner`, and for a real invite (owner = host) it
produces the error 0x80C81010.

Two readings of the live observation, both consistent with the code:

1. **The GameLobbyJoinRequested callback never ran.** Then there is no
   matchmaking traffic at all — exactly the observation. The UI's
   "Connecting…" is not driven by this path (plausibly the EOS lobby join:
   `docs/FINDINGS.md` shows the co-op lobby is EOS; the Steam layer only
   mirrors `ktol_*` metadata).
2. **The callback ran.** Then the only Steam traffic would be
   `RequestLobbyData` + `GetLobbyOwner`; a trace that only counted
   join/create traffic would still report "none", and the invite state would
   go to 3 and raise 0x80C81010 — a fast failure, not a hang. No `JoinLobby`
   in either case.

What would have to be true for the game to join: a join task (0x140D73380 or
0x140D73240) must be invoked with a non-null descriptor, with backend
`[+8] & 2` set and no open op in flight, and it must pass kind 1 into
0x1405E9440 → 0x1405F71F0. To make the *invite* work, the invite handler would
have to call (or post) that task explicitly; today it does not.

## 8. Search / host / join entry points (UI actions)

| action | function | evidence |
|---|---|---|
| **Search for a session** | `CSessionSearchHandle` vtable 0x141587D20; vfn2 0x140609D50, helper 0x140609EA0; result CallResult 0x14060A3B0 | filter at 0x14060A151/0x14060A158 (`ktol_lock_status` / `ktol_session_unlock`), `RequestLobbyList` 0x14060A1D9; result handler reads `ktol_owner_ping_location` 0x14060A4E9, `integer_` 0x14060A5BD, `binary_` 0x14060A64B (matched by string scan of 0x14060A3B0-0x14060A700). Names from `docs/RE_KTOL_KEYS.md` §3.1/§4.1 |
| **Host a session** | 0x140D71E00 (dispatches internally to Steam or EOS), launched from task step 0x140D6538D; alternate host path 0x140D74170 | kind 0 descriptor 0x140D724A1; Steam call 0x140D724D8; EOS call 0x140D724C5 (`0x1405BF3F0`); 0x140D74170 reached at 0x140D653CE |
| **Join from a search result** | 0x140D73240 (Steam branch calls 0x1405E9B20, then 0x140D732DF → 0x1405E9440) | descriptor kind `movl $0x1,0x20(%rsp)` at 0x140D732D2 |
| **Join by lobby id** | 0x140D73380 (Steam) / 0x140D73480 (EOS) | kind 1 at 0x140D733D3; JoinLobby chain §4.7 |
| **Accept/refresh a Steam invite** | 0x1405EF7E0 (handler), 0x1405EF820 (data update) | §4.9 — no join |

None of the task-layer functions reference a string literal (string-xref scan
of 0x140D71E00-0x140D72536, 0x140D73240-0x140D7337C, 0x140D73380-0x140D73419:
empty). The ktol strings live only in 0x140604F20 (publisher), 0x14060A3B0
(search result), 0x140603C80 (`ktol_common_key`, 0x140603D64) and the EOS
family (`docs/RE_KTOL_KEYS.md` §3).

## 9. Explicit absences

1. **No direct `e8` caller of `CSessionP2pOpen::vfn2` (0x1405F6780) or
   `CSessionCreateHandle::vfn2` (0x1406047F0).** Both addresses occur exactly
   once in the image, as vtable slot 2. No code refers to either slot address.
2. **No `JoinLobby` outside 0x1406038F2 and 0x140603F03** in
   0x1405E0000-0x140610000. `call *0x70(rax)`/`call *0x70(reg)` count = 2
   there. (Image-wide, `ff 50 70` is a common slot on other classes and is not
   evidence.)
3. **No `CreateLobby` outside 0x140604CFA.**
4. **No `SetLobbyJoinable` (slot 34, 0x110) call site anywhere in the session
   block** — the host never sets the joinable flag, and the client never reads
   it.
5. **No "full" flag and no member-count check**: there is no call to
   `GetNumLobbyMembers` (slot 17) on the matchmaking interface at all, and
   nothing compares a count against `GetLobbyMemberLimit` (0x140603CD4) or
   against the request's max-members field.
6. **No `ktol_lock_status` read in the join path** (see `docs/RE_KTOL_KEYS.md`
   §5); the only lock gates are the Steam and EOS search filters.
7. **`ktol_invite_data` has exactly two xrefs** (0x1405F0418, 0x1406051A3);
   neither is in the open/join path and neither compares the blob.
8. **No writer of `[CSessionP2pSteam+8]` bit 1** and **no writer of
   `[ctx+0x18]+0x4EA`** inside 0x1405E0000-0x140610000.
9. **No path from the invite/data-update handlers (0x1405EF7E0, 0x1405EF820) to
   the open entry (0x1405E9440).** Their only callees are
   `SteamInternal_ContextInit`, matchmaking slots 28/35, `GetTickCount`, the
   trace helper 0x1405ED9B0, and vtable calls back into the same object.
10. **`CSessionP2pPacketHandler` has no classmap entry / RTTI vtable**; its
    only statically visible form is the embedded 0x28-byte callback at
    `CSessionP2pSteam+0x168` (vtable 0x141587368, handler 0x1405FC0D0, id 507).
11. **The seven "invite state" helpers 0x1405EE010/0x1405EE020/0x1405EE030/
    0x1405EE040/0x1405EE050/0x1405EE060/0x1405EE070 have no callers and no
    pointer references** — they are unused in this build (or fully inlined).

## 10. Confidence

| Item | Confidence |
|---|---|
| Vtables, slot order, method VAs; single-occurrence scan | **verified** (classmap + pointer scan + constructors) |
| Static object block 0x1417F9600… and `0x1417F96C0 = CSessionP2pOpen` | **verified** (data dump + vtable stores) |
| 0x142C96250 = `CSessionP2pSteam*`; layout offsets | **verified** (ctor 0x1405EE720, factory 0x1405EE200) |
| Create path: 0x1405E9440 → 0x1405F71F0 → attach 0x1405EFB50; then per frame 0x1405F67B7 → 0x1405F6B4F → 0x1405EFA55 → 0x1406048C6 → CreateLobby 0x140604CFA | **verified** |
| Join path: 0x1405E9440 → … → `CSessionJoinHandle::vfn2` state 1 → JoinLobby 0x1406038F2 | **verified** |
| Mode field `[ctx+0x10]+8` written only at 0x1405F72B4 / 0x1405F7328 | **verified** (byte/instruction scan) |
| Invite handler performs only `RequestLobbyData`; data-update handler only sets +0x254/+0x258 | **verified** |
| State 3 → 0x80C81010 (and 5 s grace for state 1) | **verified** |
| Slot names 13/14/15/28 (Create/Join/Leave/RequestLobbyData) | **verified** by argument shape + anchored slots 19/20/35 |
| Slot 32 `GetLobbyMemberLimit` | **inferred** (SDK order, consistent argument: lobby id in `rdx`, result used as an allocation count) |
| Slot 17 `GetNumLobbyMembers` never called | **verified absence** (only two `call *0x88` sites, both bool-returning dead code) |
| Roles of 0x1405E9440 / 0x1405F71F0 / 0x140604940 / 0x140D73380 (names only; behaviour is verified) | **inferred** labels |
| "Connecting…" UI is the EOS path, not this one | **inferred** (code proves Steam cannot join from an invite; `docs/FINDINGS.md` proves co-op uses EOS) |
| The `0x5ef800` frame in the live backtrace | **unresolved**: resolves to the invite handler (0x1405EF7E0), which is not on the poll chain; stale slot or walker artefact |
