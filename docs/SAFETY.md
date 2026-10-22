# Why this mod is safe

This page explains what the mod does to your game and what it never does. It
takes about three minutes to read.

## What the mod is

The mod is one file, `dinput8.dll`. Nioh loads this file before the game
starts, because the game needs DirectInput for its controls. Windows looks for
`dinput8.dll` in the game folder first. The mod replaces that file and passes
every DirectInput call to the real Windows file.

The second file, `nioh3p.ini`, is a text configuration. It sets the player
count and the log options.

## What the mod changes

The mod changes four things inside the game process. It changes nothing else.

1. It raises the lobby member limit from 2 to 3. It does this in the requests
   that create the lobby, and only after it checks the exact game build.
2. It raises the host's visitor allowance from one to two. The game accepts
   the second visitor instead of rejecting the request.
3. It fixes one game function that copies lobby settings. When that function
   copies a value onto itself, the stock code replaces the value with empty
   memory. The fix keeps the value, so the next visitor can find the lobby.
4. It writes a text log. The log records which hooks the mod installed and
   what the game asked for.

## What the mod never does

The list below comes from the import table and from the source code. Each item
is easy to recheck with the tools named at the end of this page.

The mod opens no network connection of its own. It changes values in lobby
requests that the game already sends to Steam and EOS. The file imports 27
functions, and all of them come from `KERNEL32.dll`. No socket, HTTP or
download function is on that list.

The mod does not touch your saves. The game stores saves in
`Documents\KoeiTecmo\NIOH\Savedata`. The mod never opens that folder. In the
shipped configuration, the only files it writes are `nioh3p.log` and
`nioh3p.prev.log`, in the game folder. If the game folder is not writable, it
writes the log to `%TEMP%` instead. A research setting, `dump_image=1`, also
writes copies of the game memory to the game folder. The shipped
configuration sets it to 0.

The mod does not use the registry, services, drivers or scheduled tasks. It
does not start other programs. It does not inject code into other processes.
It acts only in `nioh.exe` and in the project test program `testgame.exe`. In
any other program that loads it, such as `nioh_launcher.exe`, it only passes
DirectInput calls to the real Windows file.

The mod does not read your passwords, your Steam login or your personal files.

## How the mod fails safely

The mod checks the game before it changes anything. It compares the timestamp,
the image size, the entry point and the checksum of `nioh.exe`. If one value
differs, the mod logs `unsupported nioh.exe build` and installs no hooks. The
game then runs unmodified.

Before each code change, the mod compares the original instruction bytes. If
the bytes differ, it skips that change and logs the reason.

The mod gates each lobby call on its `ApiVersion` value. If the game or the
EOS SDK sends a different value, the mod forwards the call unchanged.

The mod makes a code location writable for the change, and then sets it back
to read and execute. No location stays writable and executable.

To remove the mod, delete `dinput8.dll` and `nioh3p.ini`. The game is stock
again.

## How to check these claims yourself

1. Make sure that the file name is `dinput8.dll` and that it sits next to
   `nioh.exe`.
2. Compare the SHA-256 with the value on the download page. Run
   `certutil -hashfile dinput8.dll SHA256` in a Command Prompt in the game
   folder.
3. Open the VirusTotal report for that hash. Read the Behavior tab. No sandbox
   reports malicious activity.
4. Read the import table with any PE viewer. All imports come from
   `KERNEL32.dll`.
5. Read the source, if you have the source package. `src/proxy/nioh3p_proxy.c`
   is one C file. The functions that change the game are
   `game_create_param_set`, `install_game_metadata_guard` and
   `install_game_visitor_allowance`. The program check is in `DllMain`.

## About the antivirus warning

Some antivirus engines flag the file as a generic trojan. Six of 71 engines
did this on VirusTotal. The names are `Win64:Evo-gen`, `TR/W64.Evo`, `Wacatac.C!ml` and one
plain `Malicious` score. These names describe groups, not a known malware
family.

The report is at https://www.virustotal.com/gui/file/1003e4e73688f9e6af1bd95f3fbf71f5aeb3c756574b2ac27ecf1d7734f550cb.

The engines react to three things. The file is unsigned. The file changes code
in memory. The file name is one that malware also uses for DLL hijacking. All
three apply to this mod. No engine reports a network call, a persistence
mechanism or a sandbox finding.

If your antivirus blocks the file, restore it and exclude the game folder. If
you do not trust the file, delete it. The game starts normally again.
