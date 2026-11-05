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
- Version: `1.1.3` - matches the VERSIONINFO inside `dinput8.dll`
- Category: Gameplay
- Tags: `Co-op`, `Multiplayer`, `Utility`, `DLL`
- Summary (<= 200 chars): `Play Nioh missions with up to four players through the Torii Gate. Raises the game's own lobby size, adds the fourth player's health bar (a plain gauge for now), and comes with optional add-ons.`
- Requirements: Nioh: Complete Edition (Steam), the same game build on every
  PC. Windows 10 or 11, or Linux / Steam Deck with Proton. Every player
  installs the mod.
- Permissions: players may use the mod and share the page link; no re-uploads,
  no monetization, no paid mod packs.
- Live page (draft, unpublished): https://www.nexusmods.com/nioh/mods/43 -
  nine files uploaded (main `All-in-one (core and add-ons)` 1.1.3 + eight
  optionals), category Gameplay, tags Gameplay + Utilities for Players,
  description BBCode pasted, permissions preset applied, third-party = none.
  Four early optional entries show version `1` (rename/fix via their Update
  menu). Publish waits on Media images.
- Credits: `A3delta`. Source and test notes: https://github.com/Kenja-mc/nioh4p

## 2. Description (paste as BBCode)

```bbcode
[size=5]Play Nioh with up to four players[/size]
Nioh's co-op is a host plus one visitor. This mod raises the game's own lobby
to four and adds the fourth player's health bar (a plain gauge for now), so
two, three or four players can take one mission through the Torii Gate
("Yokai Realm with a Companion"). Every player installs the mod; no game
files are modified.

[size=4]Install[/size]
[list=1]
[*]Close the game.
[*]In Steam: right-click Nioh, choose Manage, then Browse local files.
[*]Extract [font=Courier New]nioh4p-all.zip[/font] into that folder. It carries everything.
[*]If another mod already provides [font=Courier New]dinput8.dll[/font], use the plugin route in the included README instead of overwriting it.
[*]Start the game from Steam on every PC.
[/list]
Linux / Steam Deck: if the mod does not load, set the Steam launch option
[font=Courier New]WINEDLLOVERRIDES="dinput8=n,b" %command%[/font].

[size=4]Play[/size]
[list]
[*]Everyone: Torii Gate, Yokai Realm with a Companion, Without Conditions, Secret Word off.
[*]Host searches first; visitors search before the preparation countdown ends.
[/list]

[size=4]Check that it loaded[/size]
The log's first lines contain [font=Courier New]preflight: exe=ok[/font].
If it says [font=Courier New]exe=unsupported[/font], your game build differs -
update the game on every PC.

[size=4]Optional add-ons[/size]
Five optional changes ship in the all-in-one zip, each with its own settings
file in [font=Courier New]DLLMods[/font]: shrine reuse, visitor Kodama,
enemy-health scaling, assist-gauge drain and shrine respawn. Turn one off
with [font=Courier New]enabled=0[/font]; delete its DLL to remove it. Their
behavior in a live fight is still under test.

[size=4]Antivirus[/size]
The DLL is unsigned and patches game memory inside the game's own process,
so some scanners flag it generically (currently 8 of 75 engines, all
heuristics). VirusTotal: https://www.virustotal.com/gui/file/2d1cdddce6134fefd019f18da62848fd969394aa35d6094c004ef4ab102655e5. If yours blocks it, keep protection on and
report the detection - or delete the mod files and the game is stock again.

[size=4]Limits[/size]
[list]
[*]The fourth health bar is a plain gauge: no name, and its fill does not track its own companion.
[*]The in-mission status card still shows only one companion.
[*]Internet play with four Steam accounts, mission completion and revives are unproven.
[/list]

[size=4]Uninstall[/size]
Delete [font=Courier New]dinput8.dll[/font], [font=Courier New]nioh3p.ini[/font],
[font=Courier New]DLLMods\nioh4p*[/font] and [font=Courier New]nioh3p*.log[/font].
Keep your save backup: modded play can save game changes, for example
collected Kodama.

[size=2]Not affiliated with or endorsed by Koei Tecmo or Team Ninja.[/size]
```

## 3. Files tab

- Main file: `nioh4p-all.zip` (version `1.1.3`, category Main). One zip, no
  nested archives.
- Optional files: `nioh4p-player.zip` (core only) and the five
  `nioh4p-<addon>.zip` per-piece add-ons, for players who do not want the
  whole set. `nioh4p-plugin.zip` for external-loader setups.
- An optional `nioh4p-1.1.3-source.zip` with `src/`, `build.sh` and
  `tools/pechecksum.py` helps vendor reviews and users.
- Do not include dumps, logs or test tools.

## 4. Images (uploaded 2026-11-02 from test-rig captures; generic account names only)

1. Four characters together in a mission (main image) - `docs/img/four-players.png`, set as the page thumbnail.
2. The HUD showing all three companion bars (the third is the plain gauge) - `docs/img/hud-bars.png`.
3. The preparation roster (host plus three companions) - `docs/img/preparation-roster.png`, cropped to remove the rig performance overlay.
4. The visitor search list showing the host session.
5. First lines of `nioh3p.log` with the `preflight:` line. Nothing needs
   blurring; the line contains only paths.

## 5. Pre-publish checklist

- [ ] Freeze the release hash: core `dinput8.dll` SHA-256
      `2d1cdddce6134fefd019f18da62848fd969394aa35d6094c004ef4ab102655e5`.
- [ ] Scan that DLL on VirusTotal and put the link in the description. Expect
      a handful of generic-heuristic flags like the predecessor's 5/71. The
      `virustotal-scan` workflow uploads every release zip and DLL
      automatically once the repository has the `VT_API_KEY` secret; its run
      summary carries the report links.
- [ ] File the false-positive reports and keep the ticket numbers. Bitdefender
      — the engine behind seven of the eight flags — confirmed the false
      positive on 2026-11-04; Cynet is still open. Drafts in
      `docs/FP_REPORTS.md`.
- [ ] Ask one Windows friend to install the mod and send their `preflight:`
      line.
- [ ] Set page permissions and credits; proofread the description.
- [x] Add "not affiliated with Koei Tecmo or Team Ninja" to the page
      (BBCode footer) and to both READMEs.
- [ ] Nexus may quarantine the first upload because of the scan hits: email
      `support@nexusmods.com` with the mod page link, the VirusTotal link and
      the source repository. Do not delete the file.

## 6. Versioning

Bump the DLL version (`src/proxy/nioh3p.rc`) with every release that changes a
binary, and keep it equal to the Nexus page version. A rebuild makes a new
hash and a new Nexus file entry; old files stay up for the users who have
them.

## 7. Automation (GitHub Actions, dormant until armed)

Two workflows sit next to `ci.yml` in `.github/workflows/`.

- `virustotal-scan` — fires on every release publish, or is dispatched with a
  tag: scans each release zip and the DLLs inside them through the free
  public API and posts a verdict table with report links to the run summary.
  Without the `VT_API_KEY` secret it warns and does nothing.
- `nexusmods-upload` — manual only, and armed only when the repository
  variable `NEXUS_UPLOADS_ENABLED` is `true` and the `NEXUS_API_KEY` secret
  is set. It downloads one release asset, prints its SHA-256, and adds it as
  a new version of an existing file entry (`file_id` input; optional
  description, changelog and page-version bump). The v3 upload API is beta
  and cannot create file entries, so first uploads stay on the web UI (§1,
  §6); use the workflow for re-uploads and version bumps of the existing
  entries.
