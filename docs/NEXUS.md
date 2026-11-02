# Nexus Mods upload kit

Everything needed to publish the player build on `nexusmods.com/nioh`.
Fill each `<…>` placeholder before publishing.

## 0. What is verified

| Item | Status |
|---|---|
| Four players in one Torii Gate mission on the test rig (four local clients) | Yes - both formations stable, three companion bars on every screen |
| Harness (player and research mode, filter-drop path, ownership, enemy-health) | Pass |
| Host name gates, unsupported-build switch-off, preflight line | Pass |
| Saves untouched by install and removal | Yes - checksums before and after are identical |
| Real Windows | Windows 11 VM: the release zip, the harness and `nioh.exe` loaded the mod with Defender at defaults and Smart App Control on, 0 detections; no gameplay in the VM |
| Play over the internet with real Steam accounts | No |
| Mission completion, deaths, revives, rewards | No |
| The add-ons' behavior in a live fight | No - load and settings publication are verified |

## 1. Page metadata

- Game: Nioh (Complete Edition), Nexus game id `nioh`
- Mod name: `Four Player Co-op (Torii Gate)`
- Version: `1.1.2` - matches the VERSIONINFO inside `dinput8.dll`
- Category: Gameplay
- Tags: `Co-op`, `Multiplayer`, `Utility`, `DLL`
- Summary (<= 200 chars): `Play Nioh missions with up to four players through the Torii Gate. Raises the game's own lobby size, adds the fourth player's health bar, and comes with optional gameplay add-ons.`
- Requirements: Nioh: Complete Edition (Steam), the same game build on every
  PC. Windows 10 or 11, or Linux / Steam Deck with Proton. Every player
  installs the mod.
- Permissions: players may use the mod and share the page link; no re-uploads,
  no monetization, no paid mod packs.
- Credits: `<your name>`. Source and test notes: `<repo URL>`.

## 2. Description (paste as BBCode)

```bbcode
[size=5]Play Nioh with up to four players[/size]
Nioh's co-op is a host plus one visitor. This mod raises the game's own lobby
size to four and adds the health bars the game is missing, so a group of two,
three or four can enter one mission through the Torii Gate ("Yokai Realm with
a Companion"). Every player installs the mod. No game files are modified.

[size=4]Install[/size]
[list=1]
[*]Close the game.
[*]In Steam: right-click Nioh, choose Manage, then Browse local files.
[*]Extract [font=Courier New]nioh4p-all.zip[/font] into that folder. It carries everything, laid out for the game directory.
[*]If another mod already provides [font=Courier New]dinput8.dll[/font], read the plugin section of the included README instead of overwriting it.
[*]Start the game from Steam on every PC.
[/list]
The defaults fit four players, and a three-player group can leave everything
as it is. Linux / Steam Deck: if the mod does not load, set the Steam launch
option [font=Courier New]WINEDLLOVERRIDES="dinput8=n,b" %command%[/font].

[size=4]Play[/size]
[list]
[*]Pick a mission that everyone owns.
[*]Host: Torii Gate, then Yokai Realm with a Companion. Search first.
[*]Visitors: the same menu. Search before the preparation countdown ends.
[*]Keep Secret Word off and use Without Conditions.
[*]You should all land in the same mission and see each other's health bars.
[/list]

[size=4]Optional add-ons[/size]
The all-in-one zip includes five optional gameplay changes, each with its own
settings file in [font=Courier New]DLLMods[/font]:
[list]
[*][font=Courier New]nioh4p-shrines[/font] - use a shrine again during co-op.
[*][font=Courier New]nioh4p-kodama[/font] - visitors record Kodama through the native save routine.
[*][font=Courier New]nioh4p-enemy-health[/font] - scale enemy health for a bigger group.
[*][font=Courier New]nioh4p-assist-gauge[/font] - adjust how fast the shared gauge drains.
[*][font=Courier New]nioh4p-shrine-respawn[/font] - self-recovery returns you to the last shrine you used.
[/list]
Each one can be turned off with [font=Courier New]enabled=0[/font] in its settings file; delete its DLL to remove it.

[size=4]Check that it loaded[/size]
The log's first lines contain [font=Courier New]preflight: exe=ok[/font]. If
it says [font=Courier New]exe=unsupported[/font], your game build differs from
the tested one and the mod left the game unmodified - update the game on every
PC.

[size=4]Antivirus[/size]
The DLL is unsigned and patches game memory inside its own process, so some
scanners flag it generically: the predecessor build scored 5 detections out of
71 engines, all generic heuristics, with no sandbox findings. If your
antivirus blocks it, restore the file and exclude the game folder - or delete
the mod files and the game is stock again. VirusTotal: `<link>`

[size=4]Known limits[/size]
[list]
[*]The preparation screen lists all four players; the in-mission status card still shows only one companion.
[*]Joining after the mission started is unproven. Steam invites are unproven.
[*]Mission completion, rewards and the revive flow with four players are unproven.
[/list]

[size=4]Uninstall[/size]
Delete the files you installed: [font=Courier New]dinput8.dll[/font],
[font=Courier New]nioh3p.ini[/font], the [font=Courier New]DLLMods\nioh4p*[/font]
files and [font=Courier New]nioh3p*.log[/font]. Keep the save backup you made:
modded play can save game changes, for example collected Kodama.
```

## 3. Files tab

- Main file: `nioh4p-all.zip` (version `1.1.2`, category Main). One zip, no
  nested archives.
- Optional files: `nioh4p-player.zip` (core only) and the five
  `nioh4p-<addon>.zip` per-piece add-ons, for players who do not want the
  whole set. `nioh4p-plugin.zip` for external-loader setups.
- An optional `nioh4p-1.1.2-source.zip` with `src/`, `build.sh` and
  `tools/pechecksum.py` helps vendor reviews and users.
- Do not include dumps, logs or test tools.

## 4. Images (capture fresh; rig screenshots show test accounts)

1. Four characters together in a mission (main image).
2. The HUD showing all three companion bars.
3. The preparation screen listing four players.
4. The visitor search list showing the host session.
5. First lines of `nioh3p.log` with the `preflight:` line. Nothing needs
   blurring; the line contains only paths.

## 5. Pre-publish checklist

- [ ] Freeze the release hash: core `dinput8.dll` SHA-256
      `ac803dd69f4fb666f62aa1caf51eddf417acaca4a9a9af1908741a01f3581b7f`.
- [ ] Scan that DLL on VirusTotal and put the link in the description. Expect
      a handful of generic-heuristic flags like the predecessor's 5/71. The
      `virustotal-scan` workflow uploads every release zip and DLL
      automatically once the repository has the `VT_API_KEY` secret; its run
      summary carries the report links.
- [ ] File the false-positive reports (Avast/AVG, Avira, WithSecure, Cynet -
      drafts in `docs/FP_REPORTS.md`) and keep the ticket numbers.
- [ ] Ask one Windows friend to install the mod and send their `preflight:`
      line.
- [ ] Set page permissions and credits; proofread the description.
- [ ] Add "not affiliated with Koei Tecmo or Team Ninja" to the page.
- [ ] Nexus may quarantine the first upload because of the scan hits: email
      `support@nexusmods.com` with the mod page link, the VirusTotal link and
      the source repository. Do not delete the file.

## 6. Versioning

Bump the DLL version (`src/proxy/nioh3p.rc`) with every release that changes a
binary, and keep it equal to the Nexus page version. A rebuild makes a new
hash and a new Nexus file entry; old files stay up for the users who have
them.
