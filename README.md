# nioh4p

Three- and four-player co-op for Nioh: Complete Edition on Steam.
Every player needs the same game build and core mod.

This source build is still under test. Four local instances entered one Torii
Gate mission and showed three companion bars on every screen. Four-account
Steam play, mission completion, and the optional gameplay changes remain unproven.
Released packages can differ from this source. Follow the instructions inside your package.

## Install and play

1. Back up your saves and close Nioh.
2. In Steam, open Nioh's local files.
3. Copy `dinput8.dll` and `nioh3p.ini` from the core package beside `nioh.exe`.
   If another mod uses `dinput8.dll`, read the [plugin instructions](package/README.md#install-the-core-as-a-plugin) first.
4. For four players, set these values in every player's `nioh3p.ini`:

   ```ini
   [nioh3p]
   max_players=4
   hud_row3=1
   ```

5. Start the game from Steam on every computer.
6. Open Torii Gate, then Yokai Realm with a Companion.
7. Let the host search first. Let the visitors search before the preparation countdown ends.

Use a mission that everyone can access. Keep Secret Word Off for this test.
Without Conditions is not a private search. Read the [full player instructions](package/README.md) before joining.

On Linux or Steam Deck, if the mod does not load, use this Steam launch option:

```sh
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

Optional add-ons separate shrine use, Kodama collection, enemy health, assist-gauge drain, and shrine recovery from the core.
Install only the add-ons that your group wants. Their live behavior is still under test.

The mod is unsigned. If antivirus blocks it, keep protection enabled and report the detection.
Review logs before sharing them. They can contain lobby IDs and file paths.
To uninstall, close Nioh and remove only the mod files that you installed.

[Releases](https://github.com/Kenja-mc/nioh4p/releases) ·
[Safety](docs/SAFETY.md) ·
[Source and test guidance](AGENTS.md)