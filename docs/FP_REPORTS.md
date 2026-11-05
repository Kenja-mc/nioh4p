# False-positive reports for `dinput8.dll` (Bitdefender + Cynet reported 2026-11-02; Bitdefender confirmed 2026-11-04)

The v1.1.3 player build `2d1cdddc…` is flagged by 8 of ~75 engines on
VirusTotal. All eight are one detection family, not eight opinions:

- `Gen:Variant.Yogi.106039` — Bitdefender's engine, licensed by ALYac,
  Arcabit, Bitdefender, Emsisoft, GData, MicroWorld-eScan and VIPRE
  (7 of the 8 flags).
- `dll.unknown.yogi` — Cynet (CTX), a generic unsigned-DLL heuristic.

The v1.1.0 build was flagged by a different set (Avast/AVG, Avira,
WithSecure, Cynet); those vendors do not flag this build. Filing with
Bitdefender and Cynet covers every current flag. Bitdefender confirmed
the false positive on 2026-11-04; Cynet's report is still open. Only the
vendor can whitelist a hash.

Shared facts (attach to every report):

- File: `dinput8.dll`, 80,384 bytes, PE32+ x64 DLL, unsigned.
- SHA-256: `2d1cdddce6134fefd019f18da62848fd969394aa35d6094c004ef4ab102655e5`
- VirusTotal: https://www.virustotal.com/gui/file/2d1cdddce6134fefd019f18da62848fd969394aa35d6094c004ef4ab102655e5
- Source: https://github.com/Kenja-mc/nioh4p (MIT-style open-source mod).
- What it is: an open-source DirectInput 8 proxy for *Nioh: Complete
  Edition* (Steam appid 485510). `nioh.exe` statically imports
  `dinput8.dll`, so the proxy loads before the game entry point and
  forwards every DirectInput export to the system DLL. It raises the
  game's own Steam/EOS lobby size from 2 to 4 inside the game process
  and writes a plain-text log. It has no network code, no persistence,
  no registry access and no injection into other processes; it gates
  every patch on the host executable's PE identity and exact code bytes,
  and is inert in any other process.

## Bitdefender (covers ALYac, Arcabit, Bitdefender, Emsisoft, GData, MicroWorld-eScan, VIPRE)

Form: https://www.bitdefender.com/consumer/support/answer/29358
("Incorrect Detection: Report a False Positive"). Attach the DLL or paste
the SHA-256.

> False positive: `Gen:Variant.Yogi.106039` on an unsigned open-source game
> mod. `dinput8.dll` (SHA-256 2d1cdddc…55e5) is a DirectInput 8 proxy for
> Nioh: Complete Edition; it forwards all DirectInput exports to the system
> DLL and adjusts the game's own lobby size in the memory of its own
> process. No network, persistence or injection code. Inert in every process
> except nioh.exe. Full source: https://github.com/Kenja-mc/nioh4p. The same
> detection is shown by your engine licensees (ALYac, Arcabit, Emsisoft,
> GData, MicroWorld-eScan, VIPRE); please whitelist the hash engine-side so
> the fix reaches them.

Confirmed false positive (2026-11-04): Bitdefender replied that
`Gen:Variant.Yogi.106039` on this file is a false positive. The removal
should reach the licensees as their engines update; re-scan the hash on
VirusTotal before quoting detection counts anywhere.

## Cynet (CTX): `dll.unknown.yogi`

Channel: Cynet support, via the contact page on cynet.com (community vendor list:
https://gist.github.com/skmedix/6cdce9d6d3b464c1bd719b72d1f6bce4).
Attach the DLL, the SHA-256 and the VirusTotal link.

> False positive: `dll.unknown.yogi` is a generic heuristic on an unsigned
> open-source game mod. `dinput8.dll` (SHA-256 2d1cdddc…55e5) is a
> DirectInput 8 proxy for Nioh that patches only its own process after
> verifying the host executable's identity. No network or persistence
> code. Source: https://github.com/Kenja-mc/nioh4p.

## Microsoft Defender: `Trojan:Win32/Wacatac.C!ml` (previous builds only)

Defender at defaults showed 0 detections on the last three builds,
including this one; the Windows 11 VM check (release zip, harness and
`nioh.exe`) was clean. No report needed unless a future scan flags it.

## If Nexus Mods quarantines the file

Email support@nexusmods.com with the mod page link
(https://www.nexusmods.com/nioh/mods/43), the VirusTotal link above and
the source repository. State that the detections are generic heuristics
on an unsigned open-source DLL, that false-positive reports are filed
with Bitdefender and Cynet, and that the file will not be deleted.
Do not delete or re-upload the file while the review is open.
