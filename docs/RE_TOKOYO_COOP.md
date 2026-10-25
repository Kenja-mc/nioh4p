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
