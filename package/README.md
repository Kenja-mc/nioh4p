# Nioh: Complete Edition – three-player co-op

This lets three people play one Nioh mission together: one host and two
visitors. Everybody in the group has to install it. The game must be the Steam
version, and all three players need the same, up-to-date Steam build.

**Back up your save first.** On Windows the saves are usually in
`Documents\KoeiTecmo\NIOH\Savedata`. On Linux they are inside the Proton prefix
under
`steamapps/compatdata/485510/pfx/drive_c/users/steamuser/Documents/KoeiTecmo/NIOH/Savedata`.
The exact place can vary, so check the folder that was changed most recently.

## Install (Windows)

1. Close the game.
2. In Steam, right-click **Nioh: Complete Edition** → **Manage** →
   **Browse local files**.
3. Copy `dinput8.dll` and `nioh3p.ini` into that folder, next to `nioh.exe`.
4. Start the game from Steam as usual.

If a `dinput8.dll` is already in that folder, it belongs to another mod. Do not
overwrite it: rename it to `dinput8.dll.backup` first, and rename it back if you
remove this mod. Only one dinput8 mod can be active at a time.

To check the file you received, open a Command Prompt in the game folder and
run:

```
certutil -hashfile dinput8.dll SHA256
```

The output must match the SHA-256 that the person who sent you the files gives
you. An unsigned DLL that patches game memory may look suspicious to antivirus:
allow it only if you trust the sender and the hash matches.

## Install (Linux / Steam Deck, Proton)

1. Close the game.
2. In Steam, right-click the game → **Manage** → **Browse local files**.
3. Copy `dinput8.dll` and `nioh3p.ini` into that folder, next to `nioh.exe`.
4. Start the game. Usually nothing else is needed.

If no `nioh3p.log` appears after starting the game, tell Steam to load the DLL:
right-click the game → **Properties** → **General** → **Launch Options** and put

```
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

If you already have launch options, keep them and put
`WINEDLLOVERRIDES="dinput8=n,b"` at the front.

## What the mod changes

The mod never writes to your save folder. Apart from its log files
(`nioh3p.log`, `nioh3p.prev.log`, in the game folder or in `%TEMP%` if the game
folder is read-only), it writes nothing anywhere. It does nothing in
`nioh_launcher.exe`. If your game is a different version (for example after an
update), the mod switches itself off and the log says so; the game then runs
unmodified.

## Check that it works

Get to the title screen, then look in the game folder for `nioh3p.log`. It
should contain these two lines:

```
game capacity hook: create request is clamped before native member allocation
game visitor allowance: recruitment opens 2 visitor slot(s) (shrine setter 2)
```

If the log says `unsupported nioh.exe build`, your game is a different build.
Update it in Steam and make sure all three players have the same version.

If there is no `nioh3p.log` in the game folder on Windows, look for
`nioh3p.log` in `%TEMP%`: type `%TEMP%` in the Explorer address bar and press
Enter. If it is not there either, check that the file is named exactly
`dinput8.dll` and sits next to `nioh.exe`.

## Troubleshooting

- If the game no longer starts after installing, delete `dinput8.dll` from the
  game folder; the game starts normally again.
- Windows Defender or Smart App Control may block or quarantine an unsigned
  DLL. That is the usual cause of the game not starting after the install.
- If your Windows user name or the game path contains non-Latin characters, the
  log may be missing even though the mod is running.

## How to play together (tested way)

**Host:** start a mission, pray at a shrine, choose **Summon Visitor** and make
the offering (Ochoko Cup).

**Visitors:** go to the Torii Gate → **Random Encounters** → **Search**
(**Become a Visitor**).

A three-player session is only found because the mod removes the game's
"locked" and "full" lobby filters. Strangers can therefore see the session and
take a free slot. For a private group, all three players use the same Secret
Word (the host sets it when summoning, visitors set it on the Become a Visitor
screen). The tested game used **Secret Word Off**; if matching does not work
with a Secret Word, set it to Off and try again.

The second visitor should start searching only after the first visitor has
fully loaded in.

Known limits:

- The party list shows only one partner name.
- After a visitor leaves, a new visitor may not be able to join until the co-op
  ends (the host goes back to a shrine or finishes the mission).
- Finishing a mission, deaths and revives, and rewards with three players have
  not been fully tested yet.

## Sending logs

Close the game first. Then send `nioh3p.log` from the game folder, or from
`%TEMP%` on Windows if that is where it was written. If the problem happened in
the previous launch, send `nioh3p.prev.log` too.

Please also say who was the host, roughly what time it went wrong, and what you
saw on screen.

The logs contain Steam lobby IDs, the game's lobby settings and search filters
(this can include the Secret Word you used), and file paths on your PC.

## Uninstall

Delete `dinput8.dll`, `nioh3p.ini`, `nioh3p.log`, `nioh3p.prev.log` (the log of
the previous launch) and any `nioh3p_image_*` dumps from the game folder. If you
renamed another mod's `dinput8.dll` during the install, rename it back. The game
is stock again.

Verifying the game files in Steam does not remove the mod (it only restores
missing or changed game files), so delete the files above yourself.
