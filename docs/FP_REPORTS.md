# False-positive reports for `dinput8.dll` (submitted 2026-10-21)

Six vendors flag the player build.
All five flags are generic heuristics.
No sandbox found malicious behavior.
Submit the text below to each vendor form with the file, or with its SHA-256 and
the VirusTotal link.
Only the vendor can whitelist a hash.
If the code is signed, this type of detection does not occur.

Shared facts:

- File: `dinput8.dll`, 73,216 bytes, PE32+ x64 DLL.
- SHA-256: `1003e4e73688f9e6af1bd95f3fbf71f5aeb3c756574b2ac27ecf1d7734f550cb`
- VirusTotal: https://www.virustotal.com/gui/file/1003e4e73688f9e6af1bd95f3fbf71f5aeb3c756574b2ac27ecf1d7734f550cb
- Source: https://github.com/ (project `nioh3p`). The source file is
  `src/proxy/nioh3p_proxy.c`, 2,900 lines of C. The code uses no CRT and imports
  KERNEL32 only.
- What it is: an open-source DirectInput 8 proxy for *Nioh: Complete Edition*
  (Steam appid 485510). `nioh.exe` statically imports `dinput8.dll`, so the DLL
  loads before the game entry point. The DLL forwards every DirectInput export
  to the system DLL. It raises the game's own Steam/EOS lobby size from 2 to 3,
  so a host can be joined by two visitors, and it writes a plain-text log.
- The DLL has no network code, no persistence, no registry access, no process
  injection and no strings that suggest data collection. It patches only the
  memory of the process that loaded it. It does this after it checks the PE
  identity of the executable and the exact instruction bytes. In any other
  process it does nothing at all (explicit host-name gate).
- The file is not packed. The `.text` entropy is 6.37. It has one import library
  (KERNEL32).

## Avast / AVG: `Win64:Evo-gen [Trj]`

> False positive: a generic heuristic on an unsigned open-source game mod.
> `dinput8.dll` (SHA-256 1003e4e7…50cb) is a DirectInput 8 proxy for Nioh. It
> forwards all DirectInput exports to the system DLL. It adjusts the game's own
> lobby size in the memory of its own process. It has no network, persistence or
> injection code. It is inert in every process except nioh.exe. The source is
> public (C, ~2,900 lines). VirusTotal shows 5/71 with the same generic verdicts
> and no sandbox findings. Please whitelist the hash or refine the heuristic.

## Avira: `TR/W64.Evo`

The text is the same, with the Avira verdict named in place of the Avast verdict.

## WithSecure: `Trojan.TR/W64.Evo`

The text is the same, with the WithSecure verdict named in place of the Avast verdict.

## Cynet: `Malicious (100)`

The text is the same, with the Cynet verdict named in place of the Avast verdict.

## Microsoft Defender: `Trojan:Win32/Wacatac.C!ml`

> This is a false positive from the Defender machine-learning model. The file
> (SHA-256 1003e4e73688f9e6af1bd95f3fbf71f5aeb3c756574b2ac27ecf1d7734f550cb) is an unsigned open-source DirectInput 8 proxy for Nioh. It has
> no network, persistence or injection code and is inert outside `nioh.exe`.
> The source is public. Please review the hash.

## What to include

1. The DLL, or its SHA-256 plus the VirusTotal link.
2. The source file `src/proxy/nioh3p_proxy.c`, and the "VirusTotal" section of
   `docs/FINDINGS.md`, as evidence of behavior.
3. A note that the game refuses to start after mod removal only because
   `nioh.exe` imports the DLL. Uninstalling means deleting the file.
