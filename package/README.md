# Nioh co-op live-test candidate

This candidate lets three or four players enter one mission through the Torii
Gate. The local rig reached a mission with both player counts. Mission
completion, deaths, revives and rewards still need a live Steam test.
Every player must install the same candidate and use the same Steam game build.

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

## Play through the Torii Gate

The default is three players, including the host. For four players, set
`max_players=4` in every player's `nioh3p.ini` before starting Nioh.
Keep the search filters at their shipped values.

1. Choose a mission that every player can access.
2. Open the Torii Gate from the world map on every client.
3. Choose Yokai Realm with a Companion on every client.
4. Set every client to Without Conditions.
5. Keep Secret Word Off on every client.
6. Set each visitor's story-progress limit at or above the host's progress.
7. Let the host choose Search first.
8. Start every visitor's search together, while the host is still recruiting.
9. Make sure that every player reaches the same mission and sees the others.

No Ochoko Cup is used. Join before the preparation countdown ends. Joining
after mission start is not supported by this test procedure. The HUD names
only one companion, even when more players are present. Make sure that the
companion name belongs to your group. Without Conditions is not a private lobby.

Do not use Steam invites for the first test. The direct-invite path is
experimental. If a player remains at "Waiting for mission to be chosen",
cancel that search and form the group again. A Himorogi Fragment ends the
mission for everyone and loses the user's Amrita.

## Live-test sequence

Back up each player's saves before the test. Start with three players.
Record the mission name, player count and any failure with these steps:

1. Move on each client and make sure that the other clients show that movement.
2. Defeat an enemy and make sure that all clients agree on its death.
3. Let one companion fall and revive that companion with the assist gauge.
4. Finish the mission and compare each player's rewards and return screen.
5. Form another group from the Torii Gate and enter another mission.
6. Repeat with four players only after the three-player test passes.

The shrine path remains available. The host starts a mission and offers an
Ochoko Cup at a shrine. Visitors use Torii Gate, Random Encounters, Search.
Let each visitor finish loading before the next visitor searches.

## Make sure that the mod loaded

Open `nioh3p.log` in the game folder. Make sure that it contains
`preflight: exe=ok` and `game tokoyo: 2/2 companion hook(s) installed`.
On the host, after recruitment starts, look for `game create request: raising
max members 2 -> 3` (or `4`) and the visitor allowance lines.
The preflight line alone does not prove that the companion hooks installed.

If the log contains `preflight: exe=unsupported` or an incomplete companion
hook count, stop the test and collect the log. Do not mix game or mod builds.

## Why the mod is safe

The mod passes DirectInput calls to the real Windows file. Inside the game,
it raises lobby capacity and visitor allowance and preserves copied lobby
settings. Two hooks send the host's mission choice to every companion and
keep the mission-start counter in step. It opens no network connection of
its own and never opens the save folder. The game still writes saves normally.
With the shipped configuration, the mod writes `nioh3p.log` and
`nioh3p.prev.log`. Research builds can also write memory dumps.

The full explanation, with the checks you can repeat yourself, is in
`docs/SAFETY.md` in the source repository.

## If something goes wrong

If the game does not start, delete `dinput8.dll`. The game starts normally
again.

This candidate is unsigned. Antivirus tools can flag DLLs that change game
memory. Results for older releases do not establish that this candidate is safe.
Compare its SHA-256 with the value supplied by the sender:

```bat
certutil -hashfile dinput8.dll SHA256
```

If antivirus blocks the candidate, stop and report the engine and detection
name. Do not disable protection or exclude the game folder for this test.

To send a log, close the game and send `nioh3p.log` from the game folder. Send
`nioh3p.prev.log` too if the problem happened in the earlier launch. The log
contains lobby IDs, search settings and file paths.

## Uninstall

Delete `dinput8.dll`, `nioh3p.ini` and the `nioh3p*.log` files from the game
folder. Your saves are never touched.
