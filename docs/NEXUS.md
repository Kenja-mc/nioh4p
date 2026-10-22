# Nexus Mods upload kit: prepare only, do not publish yet

This kit holds everything needed to put the player build on `nexusmods.com/nioh`.
Fill each placeholder `<…>` before you publish.
Publish only after the pre-publish checklist passes.

## 0. What is verified today

The player README in the zip answers the safety question for players. Long
version: `docs/SAFETY.md` in the repository. Link both from the mod page.

The table below records the state of each check.
A "Yes" entry passed on the rig or on a real machine.
A "No" entry is still open, and an "N/A" entry does not apply while the filters are stock.

| Item | Status |
|---|---|
| Three players in one mission in the rig (LAN, three Steam clients) | Yes. Host plus two visitors. Both joins went through the game's own search with stock lobby filters. |
| Harness (player and research mode, filter drop path, ownership) | Pass |
| Host name gates, unsupported-build switch-off, preflight line | Pass |
| Saves untouched | Yes. Checksums before and after are identical. |
| Real Windows | No. Static checks only (KERNEL32-only imports, PE32+ reloc/NX, no CRT apisets). |
| Three real Steam accounts over the internet | No |
| Mission completion, deaths and revives, rewards with three players | No |
| Community: strangers joining a published session | N/A while the filters are stock |

The release candidate is `dinput8.dll` from the zip `dist/nioh3p-player.zip`.
Its SHA-256 is
`48415ff045d11e34cb8e76ca9fc17afa2c8965b3d4f1c447ad96e01c646223a7`.
Freeze this hash for the release.
A rebuild resets the VirusTotal reputation and any vendor allowlist.

## 1. Page metadata

Use these values in the Nexus mod page fields.

- Game: Nioh (Complete Edition), Nexus game id `nioh`
- Mod name: `Three-Player Co-op (Summon Visitor x2)`
- Version: `1.0.0`
- Category: Gameplay
- Tags: `Co-op`, `Multiplayer`, `Utility`, `DLL`
- Summary (≤ 200 chars): `Lets a host and two visitors play one Nioh mission together by raising the game's own lobby size from 2 to 3. No game files are modified. It adds two files next to nioh.exe.`
- Requirements: Nioh: Complete Edition (Steam). All three PCs must run the same
  game build. Windows 10 or 11, or Linux with Proton. All three players must
  install the mod.
- Permissions:
  - Users can use the mod and share the page link.
  - Users must not re-upload the files elsewhere, monetize the mod, or bundle it
    into paid mod packs.
- Credits: `<your name>`. The mod builds on reverse-engineering notes in the
  public repository `<repo URL>`.

## 2. Description (paste as BBCode)

Paste the block below into the Nexus description editor.
The editor accepts BBCode.

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
The DLL is unsigned. The previous build had 6 detections out of 71 on
VirusTotal, all generic heuristics ("Evo"/"generic trojan") that an unsigned
DLL patching game memory receives. No sandbox found malicious behaviour. If your antivirus blocks it,
restore the file and exclude the game folder, or delete
[font=Courier New]dinput8.dll[/font] and the game is stock again.
Report: https://www.virustotal.com/gui/file/48415ff045d11e34cb8e76ca9fc17afa2c8965b3d4f1c447ad96e01c646223a7

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

Upload a single file.
Do not use a nested archive.
The main file is `nioh3p-player-1.0.0.zip`, with 3 files: `dinput8.dll`,
`nioh3p.ini` and `README.md`.
Set it as "Main files", version `1.0.0`, category `Main`.
Do not include the source tree, dumps, or test tools in the upload.
The upload can have a second, optional file: `nioh3p-1.0.0-source.zip` with
`src/`, `build.sh` and `tools/pechecksum.py`.
Published source helps vendor reviews and users.

## 4. Images (capture fresh, do not use rig screenshots)

Rig screenshots show test account names (`Nioh-LAN-A/B`), so do not publish them.
Capture on `<your account>` with two friends or a second PC.

1. Host at the shrine with the offering active, one visitor visible.
2. Three characters together in a mission (the main image).
3. The visitor search list showing the host session.
4. First lines of `nioh3p.log` with the `preflight:` line. Do not blur anything.
   The line contains only paths.

## 5. Pre-publish checklist

Run every item below before you publish.

- [ ] Freeze the release hash: `48415ff0…23a7`, or the hash of the final build.
- [ ] File the false-positive reports with Avast/AVG, Avira, WithSecure and Cynet
      (`docs/FP_REPORTS.md`). Keep the ticket numbers for the page reply.
- [ ] Scan the final DLL again on VirusTotal. Put the new link in the description.
- [ ] Ask one friend on Windows to install the mod. That friend sends their
      `preflight:` line.
- [ ] Set the page permissions and credits. Proofread the description.
- [ ] Add a note to the page: "not affiliated with Koei Tecmo or Team Ninja".
- [ ] Nexus can quarantine the first upload (red cross) because of the 5 scan
      hits. Email `support@nexusmods.com` with the mod page link, the
      VirusTotal link and the source repository. Do not delete the file.

## 6. Versioning

`1.0.0` is the build verified above.
Bump to `1.0.x` only for fixes that change the DLL.
A rebuild makes a new hash and a new Nexus file entry.
The old file stays for the users who have it.
Keep the `VERSIONINFO` inside `dinput8.dll` at the page version.
The file is `src/proxy/nioh3p.rc`.
