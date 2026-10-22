# Nioh 3-player co-op

Lets one host and two visitors play a Nioh mission together.
All three players need it installed, on the same Steam version of the game.

## Install

1. Close Nioh.
2. In Steam: right-click **Nioh: Complete Edition** → **Manage** → **Browse local files**.
3. Copy `dinput8.dll` and `nioh3p.ini` into that folder (next to `nioh.exe`).
   If a `dinput8.dll` is already there, it belongs to another mod: rename it to `dinput8.dll.backup` first.
4. Start the game from Steam.

Linux / Steam Deck: same steps. If it doesn't work, set the launch option
`WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Play

- **Host:** in a mission, pray at a shrine → **Summon Visitor** → make the offering.
- **Visitors:** Torii Gate → **Random Encounters** → **Search**.
  The second visitor searches only after the first one has loaded in.
- **Secret Word:** the Summon Visitor screen has no word field, so leave the
  visitors' Secret Word **Off** for this flow. A word only helps on the Torii
  Gate → *Yokai Realm with a Companion* path, where the host chooses it too.
- **Finding the host:** the mod keeps the game's normal search rules. If a host
  cannot be found, set `drop_lock_filter=1` in `nioh3p.ini` on the visitors (or
  `force_unlock=1` on the host).

## If something goes wrong

- **Game won't start:** delete `dinput8.dll`. Antivirus can block it.
- **Antivirus alert:** Avast, AVG and WithSecure may call this file a
  generic "Evo" trojan. That is a heuristic on any unsigned DLL that
  patches a game; 66 of 71 VirusTotal engines report it clean. Choose
  "restore" and add the game folder as an exclusion, or delete the file.
- **Send a log:** close the game and send `nioh3p.log` from the game folder
  (and `nioh3p.prev.log` if the problem was in the launch before).
  The log contains lobby IDs, search settings and file paths.
- **Did the mod load?** The log's `preflight:` line says so: `preflight: exe=ok …`
  means it loaded. If it says `exe=unsupported`, your game build differs from the
  tested one and the mod left the game unmodified.

## Uninstall

Delete `dinput8.dll`, `nioh3p.ini` and the `nioh3p*.log` files from the game folder.
Your saves are never touched.
