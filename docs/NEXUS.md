# Nexus Mods upload kit — prepare only, do not publish yet

Everything needed to put the player build on `nexusmods.com/nioh`. Fill the
placeholders `<…>` and publish only when the pre-publish checklist passes.

## 0. What is verified today

| Item | Status |
|---|---|
| Three players in one mission in the rig (LAN, three Steam clients) | **Yes** — host + two visitors, both joins through the game's own search, stock lobby filters |
| Harness (player and research mode, filter drop path, ownership) | **Pass** |
| Host name gates, unsupported-build switch-off, preflight line | **Pass** |
| Saves untouched | Yes (checksums before/after identical) |
| Real Windows | **No** — static checks only (KERNEL32-only imports, PE32+ reloc/NX, no CRT apisets) |
| Three real Steam accounts over the internet | **No** |
| Mission completion, deaths/revives, rewards with three players | **No** |
| Community: strangers joining a published session | N/A while filters are stock |

Release candidate: `dinput8.dll` SHA-256
`1003e4e73688f9e6af1bd95f3fbf71f5aeb3c756574b2ac27ecf1d7734f550cb`
(of the zip `dist/nioh3p-player.zip`). Freeze this hash for the release;
rebuilding resets VirusTotal reputation and any vendor allowlist.

## 1. Page metadata

- **Game:** Nioh (Complete Edition), Nexus game id `nioh`
- **Mod name:** `Three-Player Co-op (Summon Visitor x2)`
- **Version:** `1.0.0`
- **Category:** Gameplay
- **Tags:** `Co-op`, `Multiplayer`, `Utility`, `DLL`
- **Summary (≤ 200 chars):** `Lets a host and two visitors play one Nioh mission together by raising the game's own lobby size from 2 to 3. No game files are modified; two files next to nioh.exe.`
- **Requirements:** Nioh: Complete Edition (Steam), same game build on all
  three PCs; Windows 10/11 or Linux/Proton; all three players must install it.
- **Permissions:**
  - *Users may:* use it, share the page link.
  - *Users may not:* re-upload the files elsewhere, monetise, or bundle it into
    paid mod packs.
- **Credits:** `<your name>`; built on reverse-engineering notes in the public
  repository `<repo URL>`.

## 2. Description (paste as BBCode)

```bbcode
[size=5]Play Nioh with three people[/size]

Nioh's co-op is a host plus one visitor. This mod raises the game's own lobby
size from 2 to 3, so a host at a shrine can be joined by [b]two[/b] visitors.
It does not add players to the story: the host still starts the mission, both
visitors join as visitors through the Torii Gate.

[size=4]How it works[/size]
[list]
[*]Nioh loads [font=Courier New]dinput8.dll[/font] before its entry point. This mod is a proxy for
that DLL: it forwards all DirectInput calls to the real system DLL and, inside
the game, raises the lobby member limit at the two places the game sets it.
[*]It also raises the host's visitor allowance from one to two, so the game
accepts the second visitor instead of rejecting the request.
[*]The game's own search rules are left alone: locked and full sessions stay
hidden, exactly as in the unmodded game.
[*]The only file it writes is a plain-text log ([font=Courier New]nioh3p.log[/font]).
[/list]

[size=4]Install[/size]
[list=1]
[*]Close the game.
[*]In Steam: right-click Nioh → Manage → Browse local files.
[*]Copy [font=Courier New]dinput8.dll[/font] and [font=Courier New]nioh3p.ini[/font] next to [font=Courier New]nioh.exe[/font].
[*]If another [font=Courier New]dinput8.dll[/font] is already there, rename it first — only one
dinput8 mod can be active.
[*]Start the game from Steam.
[/list]
Linux / Steam Deck: same steps; if the log does not appear, set the launch
option [font=Courier New]WINEDLLOVERRIDES="dinput8=n,b" %command%[/font].

[size=4]Play[/size]
[list]
[*][b]Host:[/b] start a mission, pray at the starting shrine, choose
[b]Summon Visitor[/b], make the offering.
[*][b]Visitors:[/b] Torii Gate → [b]Random Encounters[/b] → [b]Search[/b].
[*]The second visitor should start searching only after the first has loaded in.
[*]Secret Word: the Summon Visitor screen has no word field, so leave the
visitors' Secret Word [b]Off[/b] for this flow.
[/list]

[size=4]Check that it loaded[/size]
The log's first lines must contain:

[code]preflight: exe=ok eos=3/3 steam=8 game_patch=ok log=…[/code]

If it says [font=Courier New]exe=unsupported[/font], your game build differs from the tested one and
the mod left the game unmodified — update the game on all three PCs.

[size=4]Antivirus[/size]
The DLL is unsigned. VirusTotal reports 5 detections out of 71, all generic
heuristics ("Evo"/"generic trojan") that any unsigned DLL patching game memory
receives; no sandbox found malicious behaviour. If your antivirus blocks it,
restore the file and exclude the game folder, or delete
[font=Courier New]dinput8.dll[/font] and the game is stock again.
Report: https://www.virustotal.com/gui/file/1003e4e73688f9e6af1bd95f3fbf71f5aeb3c756574b2ac27ecf1d7734f550cb

[size=4]Known limits[/size]
[list]
[*]The party list shows only one partner name.
[*]After a visitor leaves, a new visitor may not be able to join until the
co-op ends.
[*]Mission completion, deaths, revives and rewards with three players are
untested.
[*]Using two dinput8 mods at once is impossible.
[/list]

[size=4]Uninstall[/size]
Delete [font=Courier New]dinput8.dll[/font], [font=Courier New]nioh3p.ini[/font] and [font=Courier New]nioh3p*.log[/font] from the game folder. Saves are
never touched.
```

## 3. Files tab

Single file, no nested archive:

- `nioh3p-player-1.0.0.zip` (3 files: `dinput8.dll`, `nioh3p.ini`,
  `README.md`) — "Main files", version `1.0.0`, category `Main`.
- Do not include the source tree, dumps, or test tools in the upload.
- Optional second file: `nioh3p-1.0.0-source.zip` with `src/`, `build.sh`,
  `tools/pechecksum.py` (source publication helps vendor reviews and users).

## 4. Images (capture fresh, do not use rig screenshots)

Rig screenshots show test account names (`Nioh-LAN-A/B`) and should not be
published. Capture on `<your account>` with two friends or a second PC:

1. Host at the shrine with the offering active, one visitor visible.
2. Three characters together in a mission (the money shot).
3. The visitor search list showing the host session.
4. `nioh3p.log` first lines with the `preflight:` line (blur nothing — it
   contains only paths).

## 5. Pre-publish checklist

- [ ] Release hash frozen: `1003e4e7…0cb` (or the hash of the final build).
- [ ] False-positive reports filed with Avast/AVG, Avira, WithSecure, Cynet
      (`docs/FP_REPORTS.md`); keep the ticket numbers for the page reply.
- [ ] Re-scan the final DLL on VirusTotal and put the fresh link in the
      description.
- [ ] One friend on Windows installs it and sends their `preflight:` line.
- [ ] Mod page permissions and credits set; description proofread.
- [ ] Note in the page: "not affiliated with Koei Tecmo or Team Ninja".
- [ ] Nexus may quarantine the first upload (red cross) because of the 5 scan
      hits: email `support@nexusmods.com` with the mod page link, the
      VirusTotal link and the source repository; do not delete the file.

## 6. Versioning

- `1.0.0` — the build verified above.
- Bump to `1.0.x` only for fixes that change the DLL; a rebuild means a new
  hash and a new Nexus file entry, and the old file stays for users who have it.
- Keep `dinput8.dll`'s embedded `VERSIONINFO` in step with the page version
  (`src/proxy/nioh3p.rc`).
