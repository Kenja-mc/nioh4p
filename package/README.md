# nioh4p player instructions

The core raises the co-op limit to three or four players.
Every player needs the same game build and core mod.
Use matching add-ons across the group.

This source build is still under test. Four local instances entered one mission with three companion bars on each screen.
Four-account Steam play, mission completion, and the gameplay add-ons remain unproven.
The preparation screen lists all four players. Companion information and results still use the limited stock layout.
Do not treat this build as a finished release.

## Install the core

1. Back up your saves and close Nioh.
2. In Steam, right-click Nioh: Complete Edition. Choose Manage, then Browse local files.
3. Copy `dinput8.dll` and `nioh3p.ini` from `nioh4p-player.zip` beside `nioh.exe`.
   If another mod uses `dinput8.dll`, use the plugin route below instead of overwriting it.
4. For four players, set these values in every player's `nioh3p.ini`:

   ```ini
   [nioh3p]
   max_players=4
   hud_row3=1
   ```

5. Start Nioh from Steam on every computer.

The supplied configuration uses three players and disables the experimental third companion bar.
On Linux or Steam Deck, if the mod does not load, use this Steam launch option:

```sh
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

## Install the core as a plugin

If you already use a DLL loader, use `nioh4p-plugin.zip` instead of the standalone core.
Copy its `DLLMods/nioh4p.dll` into the game's `DLLMods` folder.
Set your loader to load that file. Copy `nioh3p.ini` beside `nioh.exe`, not beside the plugin.
Install only one copy of the core. This package includes no external loader.
Compatibility with a specific external loader is not proven.

## Add optional gameplay changes

Each add-on has its own DLL and configuration file in `DLLMods` beside the game.
The core loads these add-ons. No separate loader is needed for them.
Their purposes are listed below. Their gameplay behavior still needs live proof.

| Add-on | Purpose |
| --- | --- |
| `nioh4p-shrines` | Repeat shrine use during co-op |
| `nioh4p-kodama` | Let shrine visitors record Kodama through the native save routine |
| `nioh4p-enemy-health` | Adjust enemy health |
| `nioh4p-assist-gauge` | Adjust continuous assist-gauge drain |
| `nioh4p-shrine-respawn` | Return self-recovery to the last shrine used |

1. Close Nioh.
2. Copy the chosen add-on's DLL and INI from its zip into the game's `DLLMods` folder.
3. Set its configuration, then restart the game on every computer.

Each add-on reads the `[nioh4p]` section in its own INI.
Set `enabled=0` to disable it. Missing keys use the supplied defaults.
Changes take effect after a restart.
The host controls these rules during a session:

| File in `DLLMods` | Key | Default | Allowed values |
| --- | --- | --- | --- |
| `nioh4p-enemy-health.ini` | `enemy_hp_percent` | `100` | `25` to `400` |
| `nioh4p-assist-gauge.ini` | `assist_drain_percent` | `100` | `0` to `1000` |
| `nioh4p-shrine-respawn.ini` | `respawn_at_shrine` | `1` | `0` or `1` |

At `100`, the percentage rules keep normal values. At `0`, continuous assist-gauge drain is disabled.
Death, revive and recovery keep their native gauge costs.
Without an active shrine-respawn add-on, the core keeps normal recovery.
A shrine can lack the Pray prompt despite the shrine hooks. Checkpoint behavior across missions is also unproven.

## Form a Torii Gate group

1. Choose a mission that every player can access.
2. Open Torii Gate, then Yokai Realm with a Companion on every computer.
3. Choose Without Conditions and keep Secret Word Off.
4. Set each visitor's story-progress limit at or above the host's progress.
5. Let the host search first.
6. Let every visitor search before the preparation countdown ends.
7. Make sure that everyone enters the same mission and sees the other players.

The host can select a fixed mission before searching. Visitors can use Without Conditions.
This search is not private. Joining after the mission starts is not proven.
Steam invites and other co-op modes need separate tests.
A Himorogi Fragment ends the mission for everyone and loses the user's Amrita.

## Problems and removal

If the game fails to start, close it and remove only the mod files that you installed.
Restore any other mod that you backed up. Keep your save backup because the game can save changes during modded play.

Open `nioh3p.log` in the game folder. If it reports an unsupported executable, incomplete hooks, or `INCOMPLETE`, stop testing.
Review this log and `nioh3p.prev.log` before sharing them. They can contain lobby IDs and file paths.
If antivirus blocks a DLL, keep protection enabled and report the engine and detection name.

To remove an add-on, close Nioh and remove that add-on's DLL from `DLLMods`.
Keep its INI if you want to retain its configuration.
To remove the core, remove its DLL and `nioh3p.ini`. Leave files from other mods in place.
The source repository's `AGENTS.md` and `docs/SAFETY.md` contain build, test, and safety details.
