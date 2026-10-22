# False-positive reports for `dinput8.dll` (submitted 2026-10-21)

Five vendors flag the player build. All five are generic heuristics; no sandbox
found malicious behaviour. Submit the text below to each vendor's form with the
file (or its SHA-256 and the VirusTotal link). Only the vendors can whitelist a
hash; code signing would prevent the class of detection.

Shared facts:

- File: `dinput8.dll`, 73,216 bytes, PE32+ x64 DLL.
- SHA-256: `9224d41531378996d36ae67ab50d9f423f41c61be73659c4226a506f682ee122`
- VirusTotal: https://www.virustotal.com/gui/file/9224d41531378996d36ae67ab50d9f423f41c61be73659c4226a506f682ee122
- Source: https://github.com/  (project `nioh3p`; source is `src/proxy/nioh3p_proxy.c`, 2,900 lines of C, no CRT, imports KERNEL32 only)
- What it is: an open-source DirectInput 8 proxy for *Nioh: Complete Edition*
  (Steam appid 485510). `nioh.exe` statically imports `dinput8.dll`, so the DLL
  is loaded before the game's entry point. It forwards every DirectInput export
  to the system DLL, raises the game's own Steam/EOS lobby size from 2 to 3 so a
  host can be joined by two visitors, and writes a plain-text log.
- It contains no network code, no persistence, no registry access, no process
  injection, no strings suggesting data collection. It only patches the memory
  of the process that loaded it, after verifying the executable's PE identity
  and the exact instruction bytes, and it does nothing at all in any other
  process (explicit host-name gate).
- Not packed: `.text` entropy 6.37; single import library (KERNEL32).

## Avast / AVG — `Win64:Evo-gen [Trj]`

> False positive: generic heuristic on an unsigned open-source game mod.
> `dinput8.dll` (SHA-256 9224d415…ee122) is a DirectInput 8 proxy for Nioh. It
> forwards all DirectInput exports to the system DLL and adjusts the game's own
> lobby size in its own process memory; it has no network, persistence or
> injection code, and it is inert in every process except `nioh.exe`. Source is
> public (C, ~2,900 lines). VirusTotal shows 5/71 with the same generic verdicts
> and no sandbox findings. Please whitelist the hash or refine the heuristic.

## Avira — `TR/W64.Evo`

Same text, naming Avira's verdict.

## WithSecure — `Trojan.TR/W64.Evo`

Same text, naming WithSecure's verdict.

## Cynet — `Malicious (100)`

Same text, naming Cynet's verdict.

## What to include

1. The DLL (or its SHA-256 plus the VirusTotal link).
2. The source file `src/proxy/nioh3p_proxy.c` and `docs/FINDINGS.md`
   ("VirusTotal" section) as evidence of behaviour.
3. A note that the game refuses to start with the mod removed only because
   `nioh.exe` imports the DLL; uninstalling means deleting the file.
