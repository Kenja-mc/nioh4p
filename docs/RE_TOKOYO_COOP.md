# Yokai Realm with a Companion (Tokoyo Coop) for three and four players

The game calls the Torii Gate mode "Yokai Realm with a Companion" Tokoyo Coop
(常世同行). It is written for a host and one companion. This file records
where the code assumes one companion, and what the proxy changes so that two
or three companions reach the mission. All addresses are for the supported
`nioh.exe` (image base `0x140000000`). They were read from the merged image
dump and checked against live research-build traces on the rig.

## Game protocol

The Steam P2P payloads are encrypted (`CPacketReliable`). The wire log cannot
name a game packet. Two functions carry the packets in the clear:

| Address | Role |
| --- | --- |
| `0x140d6a560` | `send(id, data, len, target)`. `len` is at most `0x100`. `target` 0 to 3 is one member slot. 4 or more is every connected slot except our own. The function byte-swaps the payload in place (`0x140d69ba0`). |
| `0x140d6acc0` | `dispatch(record, sender)`. `record+0x10` is the raw id (u16), `record+0x18` the payload in network byte order. The handler for id N is `table2[table1[N-1]]` (`0x140d6c2e8`, `0x140d6c228`). |

The research build logs both (`game pkt send` and `game pkt recv`, the first
40 packets of each id, then every 500th).

### Packets of the preparation phase

Observed on the rig, host B in slot 0:

| Id | Direction | Payload | Meaning |
| --- | --- | --- | --- |
| 16 | visitor to host | 4 bytes, byte 2 = request kind | join request (admission at `0x140d6b3da`) |
| 73 | host to visitor | 4 bytes | accepted into the companion session (handler `0x140d6c06f`) |
| 77 | visitor to all | 128 bytes | visitor profile. The handler copies it to one buffer, `root+0x4c`. |
| 76 | host to partner | u16 mission id | mission preview (`root+0x3dc`) |
| 74 | both ways | 1 byte | ready flag, per-slot flag `0x80` |
| 17 | host to partner | 20 bytes | mission chosen. Bytes 0-3 member mask, 4-5 mission id, byte 6 the receiver's own slot. The receiver adopts byte 6 as its slot (`0x140d7c890`) and goes to state 9. |
| 75 | both ways | 4 bytes | start flag, per-slot flag `0x100`. A visitor sends it when its loader reaches phase 7. |
| 18, 19, 20, 21 | every pair | small | load handshake. These loop over all member slots and already work for three or four players. |
| 71 | visitor to host, host to all | 8 bytes | mission flag sync (sequence, flag id, value) |

### Where the code assumes one companion

- The host sends 76, 74, 75 and 17 to "the partner". Each sender computes the
  target as `own slot == 0 ? 1 : 0`, so the host always sends to slot 1:
  `0x140d62990` (76), `0x140d62c10` (74), `0x140d62b80` (75). The menu sends
  17 through `0x140d610e0` with relative index 1 (`0x1408c6039`).
- A second companion in slot 2 therefore receives only 73. It stays on
  "Waiting for mission to be chosen".
- `Online::AddFlagWaitCoopPlayer` (`0x1408af1b0`) is the mission script's sync
  point. The host increments mission flag `0x46` (`N+0x15837c`). A visitor
  increments flag `0x47` (`N+0x158388`). `N` is `[0x14189f408]`.
  `Online::IsWaitCoopPlayer` (`0x1408b0f30`) holds the host until the visitor
  counter reaches the host counter, and holds a visitor until the host
  counter reaches the visitor counter. Flag `0x47` is one shared counter, so
  two companions push it ahead of the host. The rig showed host 1, visitors 3.
  Every client then waits on the loading screen, and the iGPU is idle.
- The visitor-side 74 and 75 senders mark their own flags on slot 1, whatever
  their real slot is. This has had no visible effect so far.
- The 77 handler keeps one visitor profile. With two companions, the second
  profile overwrites the first one (cosmetic, "Companion information").

## The proxy changes

Both are in `src/proxy/nioh3p_proxy.c` ("Torii Gate companion"). They install
when `max_players` is 3 or 4. The hooks replace whole prologue instructions
with a 14-byte absolute jump. They install at boot, when no session exists.

1. Send hook on `0x140d6a560`. On the host, during a companion session
   (`0x14191de34 == 1`), each 76, 74, 75 and 17 for slot 1 is also sent to
   slots 2 and 3. A copy of 17 gets the receiver's slot in byte 6. The copies
   come from a snapshot taken before the original send swaps the buffer.
   Sends to an empty slot fail inside the game and are ignored.
2. Wait-flag hook on `0x1408af1b0`. In Tokoyo Coop
   (`[0x1418715e0]+0x660`), a visitor (`+0x5b0 == 1`) in slot 2 or 3 does not
   increment flag `0x47`. Slot 1 keeps the counter in step with the host.
   Later companions follow the host and slot 1.

The log shows `game tokoyo: N/N companion hook(s) installed` at boot (`N`
includes the shared-world hooks below), then
`game tokoyo: packet N also sent to companion slot S` on the host and
`game tokoyo: companion slot S leaves the shared wait counter to slot 1` on
the later companions.

## Shared world: Kodama and shrine menus

Added on `feature/shared-world-interactions`. The hooks install in the same
prologue-hook table and act only in a co-op session with at least three
connected members (session-root slot records `root+slot*0xa8+0xe8 == 2`;
closed slots hold 3 and must not count). Both the Torii Gate companion path and
the shrine-visitor path qualify. Solo and stock two-player missions keep every
native gate. No new `nioh3p.ini` key is added.

### Kodama

The stock path is already mostly correct and is kept:

- `Mission::SetStrayKodama` `0x140948e50` accepts role 0, or role 1 when
  `manager+0x660` is set (Tokoyo); a shrine visitor is rejected by that native
  role gate. It finds the selected gadget in the map at `manager+0x40`, checks
  the local save with `0x1408ef1b0` and calls the native insertion
  `0x140905bc0`. It sends no packet.
- `0x140905bc0` appends the collection ID to `save+0x15c2e4/+0x15c2e8`
  (maximum 512, duplicate check). The save object is per client.
- `Gadget::CreateStrayKodama` `0x140896560` has no co-op role gate; the
  mission script decides which placements exist on each client.

The hook therefore does not replace the native decision. Only for the client
the native role gate rejects (role 1 while `manager+0x660` is clear: the
shrine visitor) it checks the selected Kodama in the gadget map (key low
32 = `0x44c`, collection ID at `DataGadget+0xb4`), checks this client's save
with the same linear scan, and only if the ID is absent repeats the native
insertion `0x140905bc0(save, id)` immediately before the call. The host and a
Tokoyo visitor keep the original order and the pure native path. The
insertion is guarded by its plaintext prologue bytes (first 16 bytes:
`41 54 48 83 ec 40 4c 63 89 e4 c2 15 00 4c 8b e1`), because executable
memory alone does not prove the function on a machine with encrypted code
pages. That ordering matches the native accept path, which inserts first and
then runs the completion tail (`0x1408edb60` writing the per-mission result
to script VM+0x328): on the reject path the tail would otherwise compute
from the pre-insertion save. `0x140905bc0` marks the save dirty itself:
`movb $1,(save)` at `0x140905dd2`, the same dirty byte the shrine setter
`0x1408ff270` writes at `0x1408ff283`. It never writes another client's save
and sends no packet. The build logs which path credited the save.

### Shrine

`Mission::IsYashiroActive` `0x1409488a0` returns false when `manager+0x626`
is set. The game sets that latch while co-op is active, which is why the host
loses the Pray prompt as soon as a companion is present. In Tokoyo it also
returns false when `manager+0x662` is set. That byte is set by the event
handler at `0x1407bf723`; **[INFERENCE]** `Gadget::SetYashiroOnlineDoneAccess`
(`0x140899120`) is what marks the shrine used and leads to it, but the event-id
path from that binding to the handler was not traced.

The hook requires the selected interaction object (kind 2),
`richdata+0x1a82 != 0` and `manager+0x629 != 0`, then clears
`manager+0x626` (and `manager+0x662` only when `manager+0x660` says Tokoyo,
because the native predicate reads `0x662` only in that case) only for the
duration of the native `IsYashiroActive` call and restores them immediately.
The binding writes its
answer into the script argument, so letting the native code compute the answer
is what makes the mission script see true; returning 1 from the detour alone
would not. The real `Mission::YashiroOpenMenu` `0x140949620` is still what
opens the menu; the proxy adds no prompt or replacement menu. A separate hook
logs that the menu weakref at `manager+0xa90` was published, for the live
probe only.

### Checkpoint for respawn

`Mission::YashiroAccessFunction` `0x140949360` already calls
`0x1408ff270(save, shrine id)` in every mode. That sets bit `1 << id` in
`save+0x15682c` and marks the save dirty. Only the non-Tokoyo branch also
stores the ID in `manager+0x114`; the Tokoyo branch skips it.

The hook records the ID after the native call only when the selected gadget is
unchanged, the ID is not `-1` and the native visited bit (masked to the five
shift bits exactly as `0x8ff270` does) is now set. It does not require the
access flag afterwards: the native call and the visited bit already prove the
shrine was used. It then mirrors the native non-Tokoyo store
`manager+0x114 = id` and publishes it through:

    int nioh3p_world_last_shrine(uint32_t *id);

The function returns 1 when a shrine was accepted and the published ID still
equals the live native checkpoint `manager+0x114` — exactly what the stock
death action would consume right now — and 0 otherwise. That equality is not
a freshness proof: shrine IDs repeat across missions (0 and 1), and whether
the game resets `manager+0x114` on mission change is untested. If the native
field is stale after a mission change, this accessor is stale with it, by
design. It is game-thread written and poll-safe.
The stock death action at `0xa84a8b` reads
`manager+0x114` and calls `0x14087de00(manager, id)`. Verified in the
disassembly: that setter walks the trees at `manager+0x60`/`manager+0x50`
keyed by the ID, and on a hit stores the position at `manager+0x600`, the
angle at `+0x610`, sets `+0x5f0=1`, writes the position marker `+0x614=-1`
and `+0x618=0x0101`; on a tree miss it writes nothing. The marker `-1` is
what distinguishes this setter from the ID-based sibling `0x87df80`, which
stores its ID at `+0x614` instead — nothing in this branch writes `+0x614`.
It is not a full respawn or enemy reset. This branch does not call any
respawn function.

### Limits and live proof still required

- The hooks only activate with three or more connected session slots, in either
  co-op mode. They do not change ordinary single-player or two-player behaviour.
- `IsYashiroActive` clears `manager+0x626` (and `manager+0x662` in Tokoyo)
  only around the native call and restores them immediately, after the
  shrine's own gates have passed. Observed call context: between the latch
  reads at `0x140948914`/`0x14094892f` the native body runs only the binding
  glue `0x140ddf4e0`/`0x140ddfd00`, neither of which references the latches,
  and every other reader and writer of `0x626`/`0x662` in text.dis is
  game-thread mission/script code. The window is a few game-thread
  instructions on one thread; a concurrent reader on another thread could
  still observe zero. Nothing is left cleared.
- `0x140905bc0` marks the save dirty itself (`save[0]=1` at `0x140905dd2`),
  like every other save-field setter; the game's normal save points still own
  persistence. The live test must prove that a visitor's credit survives a
  return to the hub and a relaunch.
- The `manager+0x114` freshness question is open: after finishing a mission
  where a shrine was used, read `manager+0x114` at the next mission's start.
  If the game clears or moves it, `nioh3p_world_last_shrine` correctly
  reports none until a new shrine is used; if the game leaves the old ID and
  the next mission reuses it, both stay stale together and the consumer sees
  what the native respawn path sees. Record which happens.
- `manager+0x662` has no clearing write in the whole binary (seven
  references: five reads, the setter at `0x1407bf723`). The predicate survives
  that by clearing it around the native call. The Tokoyo host assist branch in
  `YashiroAccessFunction` pays out only when `0x662` is clear
  (`jne 0x140949554` at `0x1409494c6`), so later uses still do not refill
  assist. Resetting that latch permanently is not done here.
- Nothing in this section is live-proven yet.

Live scenarios for the parent:

1. Four players in one Tokoyo mission, each with a different local save. Visit
   the same Kodama with A, then B, then C. Every client must still see it until
   its own collection and the log must show either the native path or the
   `0x905bc0` fallback once per client. `save+0x15c2e4` must grow only on the
   client that collected, and B/C must keep A's older IDs.
2. At one shrine with all four present, hold E on each client. Each must get
   the real menu, close it and get it again. The log must show
   `world: shrine N usable in co-op` and `world: shrine N menu opened` on every
   client, repeatedly.
3. Pray once on each client, then read `manager+0x114` and
   `nioh3p_world_last_shrine` on each: both must be the same shrine ID, and
   `save+0x15682c` must have the bit. Kill a character and check which native
   path the death uses (`0xa84a8b` or the `0xd6b2xx` packet branch). Only the
   former consumes `manager+0x114`.

## Useful state (read-only, `tools/rig/rig.py state`)

| Field | Address | Values |
| --- | --- | --- |
| loader | `[0x1418715e0]+0x5d0` | mission loader phase, 12 = in the mission |
| tokoyo | `[0x1418715e0]+0x660` | 1 in Tokoyo Coop |
| role | `[0x1418715e0]+0x5b0` | 0 host, 1 visitor |
| mode | `0x14191de34` | 1 companion session, 0 shrine visitor |
| state | `0x141790e30` (session root `+0x3c0`) | 7 waiting for the mission, 9 mission received, 14 in the mission |
| own | `0x141790e38` (root `+0x3c8`) | own member slot |
| wait, waitn | `N+0x15837c`, `N+0x158388` | host and visitor sync counters |

The mission loader is a script-driven state machine (`0x1408825d0`, called
from `Mission::UpdatePostSetup`). Phase 1 means the post-setup update has not
started. A visitor stuck in phase 1 with an idle GPU was waiting in
`IsWaitCoopPlayer` (rip samples showed the binding on the main thread).

## Handlers resolved from the dispatch tables

`table1` at `0x140d6c2e8` (bytes) and `table2` at `0x140d6c228` (dwords) map
`raw id - 1` to a handler RVA. The mapping reproduces every address already
known by hand, `16 -> 0x140d6b3da` and `73 -> 0x140d6c06f` included, so it is
also how the ids outside the documented set were named from the merged dump:

| Id | Handler | First thing it does |
| --- | --- | --- |
| 47 | `0x140d6b926` | returns early while `root+0x3c9` is set; otherwise syncs through `manager+0x730` |
| 54 | `0x140d6b9bf` | calls `0x140d5e880` with payload bytes 0-2, then sets `manager+0x6d4` |
| 56 | `0x140d6baf7` | compares payload dword 0 against `manager+0x670` and stores into `manager+0x66c`, `+0x67c` |
| 57 | `0x140d6bb3e` | reads payload dword 1 |
| 77 | `0x140d6c0e6` | copies 128 payload bytes to `root+0x4c .. +0xcb` in eight `movups`, then sets `root+0xcc = 1` |

Id 77 is the hook point for showing more than one companion: the whole profile
lives in that one 128-byte window, which is why the second companion overwrites
the first. The panel's reader of `root+0x4c` is still unknown, so a cycling
feature needs that address before it can pick a profile rather than a timer.

## Session root accessors (from the merged dump)

| Address | Effect |
| --- | --- |
| `0x140d7adc0` | `return &g_root`, the static session root at `0x141790a70` |
| `0x140d7add0` | `slot_byte(root, slot) = *(root + slot*0xa8 + 0x108)`; confirms the `0xa8` member stride seen in the join response |
| `0x140d7c940` | stores its second argument at `root+0x4e8`; the only index-like setter near the profile block, so it is the first candidate for "which companion is shown" |
| `0x140d7d8a0` | state machine over the block that starts at `root+0x4c`: reads `+0x84` (root`+0xd0`) as a phase, `+0x98` (root`+0xe4`) as a flag, and reports a code in `0..5` |
| `0x140d7ce70` | called with the root by `0x140d5de80` |

Consumers that take the address of the profile window with `lea 0x4c(%rax)`:
`0x140d6402d` (feeds `0x140d7d8a0`, then switches on its `0..5` result),
`0x140d63198` (wrapper for `0x140d7c940`), `0x140d5de84`, `0x140d633dd`,
`0x140d63459`.

The menu class that owns the panel is `TokoyoCoopPartner@Menu`, vtable
`0x14129c9e0`, with these class-specific methods ahead of the shared `Menu`
base ones: `0x140d2fbe0`, `0x140d2fc90`, `0x140d2fca0`, `0x140d2fcb0`,
`0x140d2fcc0`, `0x140d2fe40`, `0x140d2fe50`, `0x140d2fe80`, `0x140d2ff30`.
None of them touches `0x4c..0xcc` directly, so the panel reaches the profile
through the session root rather than owning a copy.

Next step for cycling: confirm at runtime whether `root+0x4e8` selects the
displayed companion, by writing 1 or 2 while two companions are connected and
watching which name the panel shows.

The research build now answers both open questions from a normal session: on
an admission request (raw 16) and on an admission reject (raw 7) it logs

```
session: recv id=16 peer=1 allow=2 mode=1 own=0 sel=0 prof=1 slot_e8=[2,2,0,0]
```

where `allow` is `root+0x39c`, `sel` is `root+0x4e8` and `prof` is the
`root+0xcc` profile-present flag. A host that admits two companions, loses one
and admits it again will show whether `allow` really stays at zero after the
departure, which decides between patching the departure store at `0x140d7b234`
and replacing the admission check with a live slot count.



## Next targets, and why they need a live client

The name pool in `analysis/symbols.txt` is useful for naming a system but not
for finding it: the strings are real (`Character::InteractionStrayKodama` at
`0x1411f4140`, `Gadget::RequestStrayKodama` at `0x141211978`) and no code in
the image references them with a `lea`, so they are a name table without
xrefs. Addresses have to come from a live pointer or a packet.

| Feature | Search target | What one session has to provide |
| --- | --- | --- |
| Kodama for everyone | `Mission::SetStrayKodama` `0x140948e50`, insertion `0x140905bc0`, `Mission::HasStrayKodama` `0x140947f60` | mechanism implemented in "Shared world"; live test must show one save count/ID set grows only on the collecting client and every other client keeps seeing its own uncollected Kodama |
| Shrines for everyone | `Mission::IsYashiroActive` `0x1409488a0`, `YashiroOpenMenu` `0x140949620`, `YashiroAccessFunction` `0x140949360` | live test must open the real menu repeatedly on all four clients and show `manager+0x114` / `nioh3p_world_last_shrine` carry the same accepted shrine ID |
| Assist gauge and revive | `CoopHeaderGauge@Menu` (`0x14129b3f8`), `CoopPlayerGauge@Menu`, `TokoyoCoopGauge@Menu` (`0x14129b3f8` is the gauge; see classmap for the other two) | one death in a three-player party: which raw id carries it, and what `sel`/`allow` do around it |
| Enemy scaling | the mission-setup reader of the party size; member records are at `root + slot*0xa8` | enemy HP at 2, 3 and 4 players, read with `rig.py state` or a memory poller |
| Rejoin | `0x140d7b234` and `root+0x39c`, plus the `integer_82` equality filter | done: the probe shows the allowance is not consumed per admission, so this is the mission-start limit, not the counter |
| Companion cycling | `root+0x4e8` (`0x140d7c940` writes it) and the `sel` field of the probe | whether `sel` changes when the panel switches companions |

Every one of these needs a client that is actually in a session. That is the
boundary of what the dump can answer.

## Party HUD: why one companion bar

Measured on 2026-10-26, three players in a Torii Gate mission (host `b` plus
visitors `a` and `c`, research build `fd6e7c07`, `trace=1`). The probe finds
live menu elements by scanning for their vtable with `scan.exe`, which is a
read-only qword search over the readable memory of `nioh.exe`.

| Class | Vtable | Instances per client | Element id (`+0x8`) |
| --- | --- | --- | --- |
| `PlayerGauge@Menu` | `0x141277e28` | 1 | `0` (own bars) |
| `TokoyoCoopGauge@Menu` | `0x14129b3f8` | 1 | `0x96` |
| `MultiPlayerGauge@Menu` | `0x1412729c0` | 2 | `0x74` and `0x75` |
| `CoopPlayerGauge@Menu` | `0x141245d78` | 0 | — |
| `CoopHeaderGauge@Menu` | `0x1412457b8` | 0 | — |

So the Tokoyo mission HUD is not the `CoopPlayerGauge` path at all. That class
belongs to the shrine-visitor HUD, which is built by `0x140afb500` (below). A
client in a companion mission never creates it, so patching it cannot add
companion bars.

The two `MultiPlayerGauge` rows are a fixed pair, not one row per player: the
host with two companions, and each visitor with a host plus one companion, all
show exactly two. Each instance carries its own animation group at `+0x60`
and shares the sprite handles at `+0x18`/`+0x20`. The update method
`0x140c1f500` switches on the element id: `0x74` reads the global pointer block
at `0x141b46ad8`, `0x75` reads the block at `0x141b46b50`, and each block points
at four animation groups `0xa0` apart. Those two blocks are hard-wired, which
means the row-to-data binding is not a loop over member slots.

Live field values on the host, `PlayerGauge` instance `0x6df680c0`:

| Offset | Value | Meaning |
| --- | --- | --- |
| `+0x60` | `0x6e374f80` | animation group pointer |
| `+0x68` | `0x65a` = 1626 | HP maximum, matches the on-screen `1626/1626` |
| `+0x70` | `0x65a` = 1626 | HP current |
| `+0x78` | float `104.0` | Ki maximum, matches the on-screen `104/104` |

The rendered host frame (`runs/hud-probe-1026/state-b.png` in `$RIG`) shows the
own HP, Ki and assist gauge, and then exactly one companion row labelled
`Nioh-LAN-C`, the last admitted companion. There is no second row and no
second name anywhere on the frame.

The row data behind those two elements is static storage, not per-session
allocation. `0x14009afa0` constructs two state objects of `0x78` bytes at
`0x141b46a90` and `0x141b46b08` (constructor `0x140c1e210`, static-init helper
`0x1410f59f0`). The `MultiPlayerGauge` constructor `0x140c1e230` binds them by
element id: `0x74` takes the first, `0x75` the second, and every other id falls
back to the first.

In the mission, the first object mirrors the shown companion's HP: `+0x8` and
`+0x10` read `0x5f6` = 1526, which is the visitors' maximum HP, while the host
has 1626. Writing 2000/500 into those fields is overwritten within seconds, so
they are an output of the HUD, not an input. The second object stays empty: its
`+0x8`/`+0x10` hold `-1` and stay there after a write.

Filling either object with synthetic values changes nothing on screen. The
visible companion row is not drawn from those fields, and no second row appears
(`runs/hud-probe-1026/baseline-rowA.png` and `rowB-filled.png` in `$RIG`).

The two live `MultiPlayerGauge` instances differ in only three places: the id at
`+0x8` (`0x74` / `0x75`), the animation group at `+0x60`, and the dword at
`+0x28` (`0x01010001` for `0x74`, `0x01010000` for `0x75`). That dword is the
next candidate for the draw gate, ahead of any element creation.

Two further measured facts:

- `root+0x4e8` (`sel`) reads `0xffffffff` in the mission on all three clients,
  so it is not the in-mission selector. The `sel=0` in the admission log comes
  from the join packet path, before the mission starts.
- Scanning for a vtable is a usable measurement: `scan.exe` found the one
  `.data` slot that holds a known global pointer, and found each instance
  exactly once per client.

### What the shrine-visitor HUD does (for reference)

That HUD *is* built by one function, `0x140afb500` (caller `0x140883d1e`). It
allocates each element through the menu widget factory `[0x14189f400]`
(`*0x28(vtable)`, size in `edx`) and attaches it with `*0x58` on the parent:

| Element id | Class | Size | Constructor |
| --- | --- | --- | --- |
| `0x113` | not identified yet | — | `0x140aedc90` |
| `0x11a` | `CoopPlayerGauge@Menu` | `0xb8` | `0x140ae7e90` |
| `0x11b` | `CoopHeaderGauge@Menu` | `0x1a8` | `0x140ae50b0` |

The base `Menu` element constructor `0x140bf30a0` stores the element id at
`+0x8` and its third argument (the value `3` at every call site above) at
`+0xc`. The virtual at vtable `+0xa8` (`0x140bf3290`) returns `+0x8`, and the
gauge init passes that to the sprite factory `0x140ddf270`, so the position
comes from the element id.

| `CoopPlayerGauge` offset / slot | Meaning |
| --- | --- |
| `+0x60` | draw/animation state machine (`0` idle, `1`..`4` seen) |
| `+0x68` | bar widget, sprite id `0x27761` |
| `+0x70` | second widget, sprite id `0xe5c34` |
| `+0x8c` | animation sub-state |
| `+0x98` | the partner character object the bar follows |
| `+0xb4` | visible flag |
| `+0x8` (`0x140ae8190`) | create the two widgets |
| `+0x18` (`0x140ae89c0`) | per frame, `(this, arg)`: reads `[+0x98]`, then `[+0x98]+0x240` and that object's `+0xb0`, and calls `0x1407b2d30` for the ratio |
| `+0x50` (`0x140ae99d0`) | visibility / ready predicate |

### Options for the extra rows

1. Native, reuse a free layout id. Add `MultiPlayerGauge` instances with ids
   the layout data already positions, if such ids exist next to `0x74`/`0x75`.
   This needs a look at the UI layout resources in the `K300` archives, which
   `tools/lnk.py` can read. Without a free id, extra instances overlap the
   existing rows because position is keyed by id.
2. Native, create and place. Create instances with the same id and move their
   sprite objects after init. Needs the sprite transform API; survives only as
   long as the game does not re-run layout.
3. Own overlay. Read each companion's HP and draw the bars from the proxy
   through a D3D11 present hook. Independent of the menu system, but it needs
   remote character discovery and its own drawing code.

The next step is the layout question in option 1, because it is offline and it
decides whether the faithful path is cheap or expensive. `docs/RIG.md` records
how to run a probe; `scan.exe` and the run above are the measurement tools.

## The proxy now shows the second companion row (2026-10-26)

Neither creation nor the layout data was needed. The hidden row only lacks two
bits in its state word. The proxy hooks the `MultiPlayerGauge` constructor
`0x140c1e230` (19 prologue bytes, relocated because the trampoline runs one
return address deeper), and for every row that is not element `0x74` it
registers the object. A worker re-applies `state |= 0x01010001` every 500 ms
while Tokoyo mode is on (`[manager+0x660]`), because the game rewrites the word
while the HUD settles. Guard: `max_players >= 3`, so a stock two-player session
and other modes keep their layout.

Verified on the rig, player build `99076046`, three players: the host logged
`game hud: companion row id=0x75 built` and every client showed two companion
rows with the right names — host `b` saw `Nioh-LAN-C` and `Nioh-LAN-A`, visitor
`a` saw `C` and `B`, visitor `c` saw `A` and `B`
(`runs/hud-probe-1026/hud-two-rows-b.png` in `$RIG`).

Limit: the HUD has exactly two rows, so at four players one companion still has
no row. Raising that needs a third element, which brings back the creation and
layout questions above.

## Party HUD: how a row owns its companion (static, 2026-10-27)

All from the merged image dump; no live run has confirmed the rewritten third
row. Offsets are `nioh.exe` RVAs unless a field is named.

State block (`0x78` bytes, A `0x1b46a90`, B `0x1b46b08`):

| Offset | Meaning |
| --- | --- |
| `+0x00` | state: 0 empty, 1 bound, 2 showing, 3 and 4 fading (dispatcher `0xc1f500`) |
| `+0x08`, `+0x10` | HP current and maximum of the shown companion |
| `+0x20` | float, -1.0 reset value (`0xc1e310`); `0xc1fb20` zeroes the static one for the id's block |
| `+0x30` | **reference node** `{character, prev, next}`, 0x18 bytes, up to `+0x48` |
| `+0x48..+0x60` | four sprite pointers |
| `+0x68`, `+0x70`, `+0x74`, `+0x76` | fade timer, shown name id, flags |

Reference node: the nodes of one character form a doubly linked list headed at
`[character+0xf40]`. `0x140753ad0(node)` pushes at the head (`old head.prev =
node; node.next = old head`), `0x140756bc0(node)` unlinks (and rewrites the head
when `prev` is NULL), and the character release at `0x1407535d7` walks the list
and zeroes every node. The static party slots (`0x18a0490`, stride `0x18`) are
nodes of the same kind. A node is linked by address; copying its bytes to
another address leaves the neighbours pointing at the old one.

Block-argument entry points (no static block used): assigner `0xc21240(row,
arg, block)` (excludes the static nodes at `0x141b46ac0` and `0x141b46b38`
only, binds through `0xc1e400(row, block, character)`, then places the row at
anchor group `0x5f` for any id other than `0x75`), `0xc1e400` (bind and
reset), `0xc1e310(block)` (unbind and reset), `0xc1e680`, `0xc20c50`,
`0xc20a00`, `0xc20580`, `0xc20300`, `0xc1fe60`. The destructor `0xc1e2d0` and
the constructor's base `0xbf30a0` use no block. Entry points that pick A or B
by element id: `0xc1eba0` init, `0xc1e9c0` release, `0xc1f500` update,
`0xc210c0` frame, `0xc1f040`, `0xc1f140`, `0xc1ea90`, `0xc1edd0`, `0xc1f480`,
`0xc1ee40`, the constructor `0xc1e230` (resets the static block) and the tail
of `0xc1fb20` (which zeroes `A+0x20` for id `0x73`).

Sprite readiness: `0x140dd93e0(sprite)` is true when `[sprite+0x28]` and
`[[sprite+0x28]+0x28]` are non-NULL and the container's `+0x2ac` counter is 0.
The class frame slot checks it; the update slot and the sprite API
(`0x140dd6130` jumps to `0x140532620` with `rcx = [[sprite+0x28]+0x28]`) do not.

Character data: `data = [character+0x240]`, maximum HP `[data+0x18]`, current
HP `[data+0x20]` (script getters `Character::GetHostHpRate` and `GetHpRate`
use the same fields). The assigner accepts a character when `+0xef0 != -1`,
`+0x430 != 0`, `data != NULL` and `[data+0x10] != 1`.

## Companion information card and the profile window: correction (static, 2026-10-28)

Read from the merged image dump only; nothing here was observed live. It
replaces three earlier guesses in this file: that the `root+0x4c` window is what
the "Companion information" panel shows, that `root+0x4e8` selects which
companion it shows, and that per-sender profile storage would fix the panel.

**The `root+0x4c..0xcb` window is mission-compatibility data, not a card.**
Packet 77 (128 bytes, `0xd6c0e6`) is built by `0x8f34d0` / `0x8eeac0` and sent
by `0xd62a30` to every other member (`target 4`) when `root+0xcc == 0`. The
layout the consumers read is: `0x74` bytes of per-mission state (cleared by the
loop in `0x8f34d0`, queried per mission id by `0x8f3d90` and `0x8f3fc0`), a
u32 minimum level at `+0x78` and a u32 maximum level at `+0x7c` (read by
`0xd2c6e0`, which narrows the host's own range to the partner's). The one
accessor is `0xd5ddf0()`: `&root[+0x4c]` when `root+0xcc == 1`, else NULL.
Its callers are the mission list builders `0xd2b410` (random mission pick),
`0xd396a0` (list filter), `0xd2c6e0` (level range), `0xd2ca30`, `0xd2d900`
and the matching state machine `0xd29ed0`. With three visitors the host's
window holds only the last sender's data, so the mission filter and the level
range follow one companion. That is a rules question (intersection of all
companions), not a display one.

**`root+0x4e8` is a dialog result, not a companion selector.** `0xd7c940`
stores it; its only writers are `0xd63190`, called from the Matching menu
(`0xd29ad0`, state 7) and from `0xc93760` with `[param+0xa4]`, the index the
player chose in a confirmation dialog. `0xd5e900` tests it for "waiting for the
answer" (`root[0]==1 && root[4]==2 && sel<0`).

**Where a companion's name and card come from.**
- Member names: `0xd5d920(relative index)` maps the index with `0xd7a0d0` and
  returns the NUL-terminated name at `root + slot*0xa8 + 0x10f` (`0xd7a090`).
  The mission HUD uses it with `[character+0xef0]` (`0xc21240`).
- Session members: `0xd66c60()+0xa8` is the lobby session object; `0xd72640
  (session, index)` returns a member, `0xd72a10(member)` its name, `0xd72b60`
  the local index. `0xd5a020` is the local name and `0xd59fc0(index)` a
  member's.
- The card is `PlayerOnlineState@Menu` (`0x141279ac8`). `0xc6c080(card, parent,
  slot, anim)` stores `slot` at `card+0x70`, `anim` at `+0x74` and runs the
  refresh `0xc6c180`. With `+0x70 == 0` it prints the local name (`0xd5d280`);
  otherwise it prints the name of **session member 1** - the immediate in
  `0xc6c4fb` is `mov $1,%ecx; call 0xd59fc0` and does not use `+0x70` - and
  reads the party character `0x755dc0(+0x70)` for level and title.
- The container `0xc83bc0(holder, slot, parent)` (holder is `[0x14189f400+0x7d8]`)
  stores `slot` at `+0x74`, resolves the character with `0x755dc0(slot)` and
  data `[char+0x240]`, picks a card variant from `+0x70` and calls `0xc6c080`
  with that slot. Its caller `0xd2fda0` passes the constant `1`: the
  `lea 0x1(%r8),%edx` at `0xd2fdd4`. So the native card is hard-wired to party
  slot 1 and session member 1, for a host and for a visitor alike.


### The member index mapping is now proven (live + static, 2026-10-28)

`0xd7a0d0(root, k)` is the game's own party-slot -> session-member map. Read
out byte for byte it is: `k == 0` returns `root[0x3c8]`; `k > root[0x3c8]`
returns `k`; otherwise `k - 1`. `root[0x3c8]` (u8) is the local player's own
absolute session index. So for party slot `k` in 1..3 the session member that
carries that companion is `m(k) = (k <= S) ? k - 1 : k` with `S = root[0x3c8]`.

Three independent confirmations:

- Live roots from the four-client prep run: the host's `root[0x3c8] = 0`
  (`b-root.bin`), the first visitor's `= 1` (`a-root.bin`).
- The HUD row name getter `0xc1ead0(char)` feeds `char+0xef0` (the character's
  party slot) straight into `0xd5d920`, i.e. the same map.
- The formula reproduces every row name the three-player mission run printed
  (docs above, 2026-10-26): host b (S=0) saw rows C and A, visitor a (S=1) saw
  B and C, visitor c (S=2) saw B and A.

The mission HUD also uses this path for the row names; the root name records
(`root + k*0xa8 + 0x10f`) were still all zero in the prep snapshots, so they
are filled in mission only. The card name instead reads the session member
array pointer (`[0xd66c60()+0xc0]`, stride 0xd0, UTF-8 name at +0x10), which
has names in prep too. The member count is NOT the u32 at manager+0xf4 (it
read 0 on all four live clients): the count is u32[session+0xc0] on the
CSessionP2p object at `[mgr+0xa8]` - the bound of the game's own name-index
scan `0xd72bb0` (`cmp %ebx,0xc0(%rcx)`, text.dis 3851335). `0xd72640` has no
bounds check, so any member index must be derived from that count, never
passed raw.

### Live prep observations (four LAN clients, 2026-10-28)

- `0x755dc0` party array (`0x1418a0490`): slots 0/1 hold characters, slots 2/3
  NULL, although all four Steam clients sat in the same lobby. The party array
  tracks admitted companions, not Steam lobby members; slots 2/3 must not be
  relied on in prep (`a-party.bin`/`b-party.bin`).
- The panel holder (`[0x14189f400+0x7d8]`, element id 0xfb at `+0x8`, live
  0x6dfcce60) with the panel open: `+0x60 = 2`, `+0x70 = 0` (status variant),
  `+0x74 = 1` (party slot) - `b-holder-open.bin`.
- `root+0x3c0 == 0xE` in prep while the screen still counts down (parent's
  screenshot: 16 s). `0xE` there is therefore NOT a mission marker. It is the
  gate the connected-slot predicate `0xd7af80` requires
  (`cmpl $0xe,0x3c0(%rcx)`), i.e. a lobby/prep phase value; the connected bits
  (record `+0x104` bit 0) were 0 for every record in both root snapshots, so
  at that instant the root records did not reflect the Steam lobby members.
- The panel the menu opens is the full status window (Core/Battle stats, 3D
  preview), the container's variant 0; the small `PlayerOnlineState` card is
  variant 4 and needs `[0x14189f420]+0x18 != 0` (0xc83c94..0xc83ca6 forces
  variant 0 when that byte is 0). No live surface with the card variant has
  been captured yet.

### The prep roster is now implemented (not live-proven)

**Where the native multiline text actually lives.** The matching/prep menus
draw their help box through one shared per-frame updater, `0xd2ce40`: four
text lines on the sprite at `this+0x68`, text slot ids from the dword tables
`0x178d2a0` (texts `0xb`, `0xc`, `0xd`, `0xe`) and `0x178d2b0` (per-line dim
flags `0x6`-`0x9`). Texts are hash-set with `0xddaee0(sprite, slot, hash)`;
raw text goes through `0xddaf90(sprite, slot, utf16, len)` - the same setter
the companion card uses - and the per-line attributes are
`0xddbf00(sprite, flagSlot, dimmed)` and `0xddb2c0(sprite, textSlot, colour)`
with the colour dwords at `0x126977c` (active) / `0x12697a0` (dimmed). The
multiline mechanism is multiple positioned slots, not `\n`. The
bottom-right companion name (`0xd2c520`) is instead an immediate-mode draw
(`0xacde50`, position inside the draw system) and also hardcodes member 1 -
not usable for a roster.

**What the proxy does.** `0xd2ce40` is inline-hooked (23-byte prologue,
RIP-relative compare rewritten absolute in the trampoline). The detour runs
the stock pass first, then applies three gates before it touches any text:
`max_players >= 3`; the Tokoyo coop mode byte (`0x14191de34`) equal to 1; and
the mission loader phase (`[manager+0x5d0]`) strictly below the in-mission
value 0xC - a positive pre-mission bound, since the mode byte is 1 in the
mission too and `root+0x3c0 == 0xE` holds in both prep and mission. Menus
outside the preparing companion session keep their stock help box. Inside the
gate, when the member count (u32[session+0xc0] on the CSessionP2p object at
`[mgr+0xa8]` - live-proven on a four-client mission; the u32 at `mgr+0xf4` is
not the count, it read 0 on all four clients) is 3 or 4, help lines 2-4 are
rewritten with the roster: the connected companions' session names, the
companion count prefixed to the first line ("3 Nioh-LAN-A"). Members come
from the game's global session member array pointer (`[0x17900b0+0xc0]`,
stride 0xd0, UTF-8 name at +0x10 - the array `0xd72640` dereferences),
skipping the local member (index `root[0x3c8]`).
Names are untrusted: bounded to 0x40 bytes, control/newline bytes dropped
before conversion, and the converted line filtered again for C0/C1 controls;
conversion and formatting use the game's own helpers (`0xddf4c0`, code page
65001; `0x7e6ee0` with a constant `"%d %ls"`-style format - the name is never
used as a format). Line 1 (the per-menu-item help) stays stock, and with
fewer than three members the whole box is stock. All five game helpers the
detour calls are signature-checked once at first use; every pointer is
readability-guarded. No input, no invented characters, no overlay. The log
records counts only - never names or platform ids.

**Card panel experiments: removed.** The panel-slot paging experiment on the
container `0xc83bc0` and the card name-index call patch at `0xc6c4fb` are both
gone, along with the `call_patch`/gate installer fields and the near-module
stub allocator they needed. Neither was ever exercised live: the prep party
array keeps slots 2/3 NULL, so a paged or renamed slot-2/3 card is not
reachable in the actual preparation screen, and no card-variant-4 surface was
ever captured. The stock Companion information window is untouched and still
shows the first companion; the roster in the help box is the surface that
shows all connected names. The static facts above (the `0xc6c4fb` and
`0xd2fdd4` hardcodes, the `m(k)` mapping) remain valid documentation for any
future card work.

Open (needs live capture before this is proven):

- The phase gate (mode 1, loader phase < 0xC) is only as good as the observed
  values: prep 0, mission 0xC. If any other in-session screen sits between
  them with its own help box, that box would show the roster too - the
  capture should note which menus ran while mode 1 / phase < 0xC held.
- Whether the card variant (4) ever shows in a captureable surface, and
  whether party slots 2/3 fill after the Ginchiyo admission or only in the
  mission.

Live probe for the roster: with 3-4 clients in the Tokoyo preparation
(mode 1, loader 0, root+0x3c0 0xE), the prep help box lines 2-4 should list
the companions with the count on the first roster line, and the log gets one
count-only line per roster change (`help roster: N companion(s) ...`). With
one or two players, in other menus, and after mission entry (loader 0xC), the
box must be stock.

The roster above deliberately reads the session member array instead of the
party array: the session array carries every connected member's name from the
moment the lobby exists, while the party array only carries admitted
companions (slots 2/3 NULL in the observed prep run).
