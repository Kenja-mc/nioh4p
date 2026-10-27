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

The log shows `game tokoyo: 2/2 companion hook(s) installed` at boot, then
`game tokoyo: packet N also sent to companion slot S` on the host and
`game tokoyo: companion slot S leaves the shared wait counter to slot 1` on
the later companions.

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
| Kodama for everyone | `Character::InteractionStrayKodama`, `Gadget::RequestStrayKodama`, `Gadget::CreateStrayKodama` | the packet produced when a visitor picks one up, and whether the flag travels as raw 71 |
| Shrines for everyone | the Pray prompt predicate; no class name found, so start from the shader/UI string and the interact radius | whether a visitor's pray sends anything, and what the host does with it |
| Assist gauge and revive | `CoopHeaderGauge@Menu` (`0x14129b3f8`), `CoopPlayerGauge@Menu`, `TokoyoCoopGauge@Menu` (`0x14129b3f8` is the gauge; see classmap for the other two) | one death in a three-player party: which raw id carries it, and what `sel`/`allow` do around it |
| Enemy scaling | the mission-setup reader of the party size; member records are at `root + slot*0xa8` | enemy HP at 2, 3 and 4 players, read with `rig.py state` or a memory poller |
| Rejoin | `0x140d7b234` and `root+0x39c`, plus the `integer_82` equality filter | done: the probe shows the allowance is not consumed per admission, so this is the mission-start limit, not the counter |
| Companion cycling | `root+0x4e8` (`0x140d7c940` writes it) and the `sel` field of the probe | whether `sel` changes when the panel switches companions |

Every one of these needs a client that is actually in a session. That is the
boundary of what the dump can answer.

## Party HUD: why one companion bar

The mission HUD builds one companion bar per screen. The bar is an instance of
`CoopPlayerGauge@Menu` (vtable `0x141245d78`), not a list, so a third and fourth
player have no bar to draw into.

The whole HUD for a Tokoyo mission is built by one function, `0x140afb500`
(caller `0x140883d1e`). It allocates each element through the menu widget
factory `[0x14189f400]` (`*0x28(vtable)`, size in `edx`) and then attaches it
with `*0x58` on the parent. The elements in that builder:

| Element id | Class | Size | Constructor |
| --- | --- | --- | --- |
| `0x113` | not identified yet | — | `0x140aedc90` |
| `0x11a` | `CoopPlayerGauge@Menu` | `0xb8` | `0x140ae7e90` |
| `0x11b` | `CoopHeaderGauge@Menu` | `0x1a8` | `0x140ae50b0` |

The base `Menu` element constructor `0x140bf30a0` stores the element id at
`+0x8` and its third argument (the value `3` at every call site above) at
`+0xc`. The virtual at vtable `+0xa8` (`0x140bf3290`) returns `+0x8`, and the
gauge's own init passes that value to the sprite factory `0x140ddf270`, so
**the screen position comes from the element id**. Two gauges created with id
`0x11a` would draw in exactly the same place.

`CoopPlayerGauge@Menu` fields and methods:

| Offset / vtable slot | Meaning |
| --- | --- |
| `+0x60` | draw/animation state machine (`0` idle, `1`..`4` seen) |
| `+0x68` | the bar widget, created with sprite id `0x27761` |
| `+0x70` | the second widget, sprite id `0xe5c34` |
| `+0x8c` | animation sub-state |
| `+0x98` | the partner character object that the bar follows |
| `+0xb4` | visible flag |
| `+0x8` (`0x140ae8190`) | create the two widgets |
| `+0x18` (`0x140ae89c0`) | per-frame: `(this, arg)`, reads `[+0x98]`, then `[+0x98]+0x240` and the status block at `+0xb0` of that, and calls `0x1407b2d30` for the ratio |
| `+0x50` (`0x140ae99d0`) | visibility / ready predicate |

So the bar shows whichever character object the owner assigns to `+0x98`.
Nothing in the gauge reads the session root, and nothing else holds a pointer
to the gauge through its vtable: the owner reaches it with virtual calls, which
is why a plain vtable xref search finds only the constructor and destructors.

Two implementation paths follow from that:

1. Native widgets. Create `max_players - 2` extra `CoopPlayerGauge` instances
   through the same factory, give each its own character object, and give each
   a distinct screen offset, because the layout data is keyed by element id.
   The offset has to be applied to the created widget objects after init. This
   reuses the game's own drawing, but needs the sprite transform API and a
   free position for each bar.
2. Own overlay. Read the character objects of the extra companions and draw
   the bars from the proxy. This skips the menu system entirely and needs
   remote actor discovery plus a D3D11 present hook.

Both need the same first datum: the live character object behind a companion's
bar. Next step is a read-only scan of a three-player client for vtable
`0x141245d78` while in the mission, then reading `[instance+0x98]` and
`[[instance+0x98]+0x240]` for each companion, and testing whether `root+0x4e8`
(`sel`) picks which companion the one bar shows.
