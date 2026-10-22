# Nioh three-player co-op

This mod lets one host and two visitors play one Nioh mission together.
All three players must install it. All three must use the same Steam build of
the game.

## Install

1. Close Nioh.
2. In Steam, right-click Nioh: Complete Edition, then choose Manage, then
   Browse local files.
3. Copy `dinput8.dll` and `nioh3p.ini` into that folder, next to `nioh.exe`.
   If a `dinput8.dll` is already there, it belongs to another mod. Rename it to
   `dinput8.dll.backup` first and do not overwrite it.
4. Start the game from Steam.

On Linux and Steam Deck, use the same steps. If no log appears, add this
launch option in Steam: `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Play

1. The host starts a mission, prays at the starting shrine, chooses Summon
   Visitor and makes the offering.
2. Each visitor goes to the Torii Gate, then Random Encounters, then Search.
3. The second visitor starts searching only after the first visitor has
   loaded in.
4. Leave the visitors' Secret Word Off. The Summon Visitor screen has no word
   field, so a word cannot match.

## Make sure that the mod loaded

Open `nioh3p.log` in the game folder. The line `preflight: exe=ok eos=3/3 …`
means that the mod loaded. The line `preflight: exe=unsupported` means that
your game build differs from the tested build. The mod then leaves the game
unmodified.

## Why the mod is safe

The mod passes every DirectInput call to the real Windows file. Inside the game
process, it changes the lobby member limit, the host visitor allowance and one
game function that copies lobby settings. It also writes a text log. It opens
no network connection of its own. It never opens the save folder. It writes
only `nioh3p.log` and `nioh3p.prev.log`. In any program except `nioh.exe`, it
only passes DirectInput calls through.

The full explanation, with the checks you can repeat yourself, is in
`docs/SAFETY.md` in the source repository.

## If something goes wrong

If the game does not start, delete `dinput8.dll`. The game starts normally
again.

Microsoft Defender, Avast, AVG, Avira and WithSecure can flag the file as a
generic trojan. That warning is a heuristic on an unsigned DLL that changes game
memory. Compare the SHA-256 with the value that you got with the download.
Then restore the file and exclude the game folder, or delete the file.

To send a log, close the game and send `nioh3p.log` from the game folder. Send
`nioh3p.prev.log` too if the problem happened in the earlier launch. The log
contains lobby IDs, search settings and file paths.

## Uninstall

Delete `dinput8.dll`, `nioh3p.ini` and the `nioh3p*.log` files from the game
folder. Your saves are never touched.
