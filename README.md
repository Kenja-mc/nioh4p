# nioh3p

nioh3p is an experimental co-op mod for Nioh: Complete Edition on Steam. It raises the player limit from two to three or four. Every player needs the same mod and game build.

Three players played a mission together over Steam. A local test also brought three and four players into a mission through the Torii Gate. Finishing a mission and reviving a companion are not yet proven. The result screen names only one companion. A four-player test showed only two companion health bars, even with the experimental third row enabled. Other co-op modes remain untested.

## Get the mod

The [release candidate](https://github.com/Kenja-mc/nioh4p/releases/tag/v1.1.0-rc2) includes Torii Gate support. The older player zip in `dist/` predates that support. Read the instructions inside the package for the build you install. The current candidate instructions are also in [package/README.md](package/README.md).

The mod targets the supported Steam game build and was tested under Proton. On an unsupported executable, the mod logs that it is inactive and leaves the game unmodified. Windows play is not established by the local Proton tests.

## Install and play

1. Back up each player's saves and close Nioh.
2. In Steam, right-click Nioh: Complete Edition. Choose Manage, then Browse local files.
3. If `dinput8.dll` already exists there, back it up before installing the mod. It belongs to another mod.
4. Copy `dinput8.dll` and `nioh3p.ini` from the same package into the folder beside `nioh.exe`.
5. Start Nioh from Steam on every player's computer.

On Linux or Steam Deck, use the same steps. If `nioh3p.log` does not appear, add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the game's Steam launch options.

The default is three players, including the host. For four players, set `max_players=4` in every player's `nioh3p.ini` before starting the game. Keep the search filters at their default values.

For the Torii Gate, all players choose Yokai Realm with a Companion, Without Conditions, and Secret Word Off. The host searches first. The visitors search while the host is still recruiting. Each visitor must set the story-progress limit at or above the host's progress. This search is not private. The [player instructions](package/README.md) cover the full sequence, the shrine path, and troubleshooting.

## Safety and removal

The mod is unsigned. Antivirus tools can flag DLLs that change game memory. Do not disable protection or exclude the game folder. If antivirus blocks the mod, stop and report the detection name. The mod does not open saves or make its own network connections, but the game still writes saves. See [safety details](docs/SAFETY.md).

The mod writes `nioh3p.log` and `nioh3p.prev.log` in the game folder by default. Logs can contain lobby IDs, search settings, and file paths. Review them before sharing.

To remove the mod, close the game and delete its `dinput8.dll`, `nioh3p.ini`, and `nioh3p*.log` files. If you backed up another mod's DLL, restore that file.

## Build and launch on Linux

From the repository root, run `tools/package.sh` to build a player zip. It writes `dist/nioh3p-player.zip` and replaces an existing zip at that path. Run `./install.sh` to build and install the mod on this machine. Set `NIOH_DIR` if the game is not in the default Steam folder. The installer backs up a different existing `nioh3p.ini` and refuses to overwrite another mod's DLL.

Run `./install.sh uninstall` to remove the installed mod files. It leaves logs in place. `./play.sh` starts the game through Steam. `./play.sh status` shows the installed state and launch options. `./play.sh stock` removes the mod, and `./play.sh mod` installs it again.

For source and research guidance, see [AGENTS.md](AGENTS.md).