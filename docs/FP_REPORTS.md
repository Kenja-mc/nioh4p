# False-positive report drafts for `dinput8.dll` (not submitted yet)

Five vendors flag the player build `48415ff0…`: Avast, AVG, Avira, WithSecure
and Cynet. All five flags are generic heuristics.
The CAPA sandbox found no malicious behavior. Zenbox and CAPE were still running.
Submit the text below to each vendor form with the file, or with its SHA-256 and
the VirusTotal link.
Only the vendor can whitelist a hash.
A code signature can help a vendor identify the publisher. No vendor source
says that a signature removes this type of detection.

Shared facts:

- File: `dinput8.dll`, 38,400 bytes, PE32+ x64 DLL.
- SHA-256: `48415ff045d11e34cb8e76ca9fc17afa2c8965b3d4f1c447ad96e01c646223a7`
- VirusTotal: https://www.virustotal.com/gui/file/48415ff045d11e34cb8e76ca9fc17afa2c8965b3d4f1c447ad96e01c646223a7
  (upload the file first if this link shows no report).
- Source: https://github.com/ (project `nioh3p`). The source file is
  `src/proxy/nioh3p_proxy.c`, about 3,150 lines of C. The code uses no CRT and imports
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
> `dinput8.dll` (SHA-256 48415ff0…23a7) is a DirectInput 8 proxy for Nioh. It
> forwards all DirectInput exports to the system DLL. It adjusts the game's own
> lobby size in the memory of its own process. It has no network, persistence or
> injection code. It is inert in every process except nioh.exe. The source is
> public (C, about 3,150 lines). VirusTotal shows only generic verdicts and no
> sandbox findings. Please whitelist the hash or refine the heuristic.

## Avira: `TR/W64.Evo`

The text is the same, with the Avira verdict named in place of the Avast verdict.

## WithSecure: `Trojan.TR/W64.Evo`

The text is the same, with the WithSecure verdict named in place of the Avast verdict.

## Cynet: `Malicious (100)`

The text is the same, with the Cynet verdict named in place of the Avast verdict.

## Microsoft Defender: `Trojan:Win32/Wacatac.C!ml` (previous build only)

Defender flagged the previous build `1003e4e7…` on VirusTotal. It reports
`48415ff0…` as clean on VirusTotal and in a Windows 11 VM. Use this text only if
Defender flags the current build later.

> This is a false positive from the Defender machine-learning model. The file
> (SHA-256 48415ff045d11e34cb8e76ca9fc17afa2c8965b3d4f1c447ad96e01c646223a7) is an unsigned open-source DirectInput 8 proxy for Nioh. It has
> no network, persistence or injection code and is inert outside `nioh.exe`.
> The source is public. Please review the hash.

## What to include

1. The DLL, or its SHA-256 plus the VirusTotal link.
2. The source file `src/proxy/nioh3p_proxy.c`, and the "VirusTotal" section of
   `docs/FINDINGS.md`, as evidence of behavior.
3. A note that the game refuses to start after mod removal only because
   `nioh.exe` imports the DLL. Uninstalling means deleting the file.
