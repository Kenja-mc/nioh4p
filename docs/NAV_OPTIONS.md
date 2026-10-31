# Faster menu and game navigation for the nioh3p rig: ranked options

Scope: this document ranks ways to move the host and the visitors from a cold
client into co-op. It also covers the return to a state you can test again.
Every option removes part of the current screenshot, OCR and fixed-sleep loop.
All measurements were read-only on this machine on 2026-10-21. No game was
launched and no repo file was changed. Everything not directly observed is
tagged **[INFERENCE]**.

---

## 0. TL;DR

| # | Option | Speedup | Effort | Risk |
|---|---|---|---|---|
| A | **Memory state oracle** (read-only, external) + deterministic key scripts | 250× per wait; nav becomes load-bound | 0.5–1 d | low |
| D | **Fingerprint vision** (template/row tests) instead of OCR | 25–45× per check (3.4 s → ~0.12 s) | 4–6 h | low |
| F | **`rig/coop.py`** state machine over A+D (+ warm-session reuse) | whole flow minutes → seconds | 1–2 d | low–med |
| C | **Save templates** per role (host at shrine / visitor on map) | 10–30 s/launch, fewer retries | 1–2 h | low |
| E | **Movie fix / skip** (`movie/EV0001.wmv`, `nioh.ini [MOVIE]`) | 109 s/boot + removes today's crash | 1 h | low (rig only) |
| B1 | **DirectInput injection** inside the proxy (input, not menus) | removes XTest/libei/XInput flakiness | 1–2 d | med |
| B2 | **In-process script-binding calls** (skip menus) | potentially 0 menu time | days | **high** — not recommended |

Recommended plan, in three steps. Step 1 ships `coop.py`: the memory oracle,
the fingerprints and the deterministic sequences (A+D+F). Step 2 adds per-role
save templates (C) and tests the movie workaround on slot C (E). Step 3 adds
B1, but only if input still flakes.

---

## 1. Baseline: what the current loop actually costs

`tools/rig/nioh_nav.py` has these costs:

| cost | where | measured / stated |
|---|---|---|
| fixed 2.0 s sleep before every screenshot | `shot()` line 108–113 | pure sleep, not needed (see §5) |
| screenshot | `gamescopectl screenshot` | **62–71 ms** end-to-end at 1280×720, PNG complete on first poll (5 runs, gamescope 3.16.31 headless + vkcube) |
| OCR preprocess | `magick -resize 200% -colorspace Gray -sharpen` | **0.656 s** |
| OCR | `tesseract --psm 6` | **0.191 s** |
| `classify()` | full frame, then top-left crop, then footer crop | up to **3 OCR passes ≈ 2.5 s** |
| `wait_state()` | classify + `time.sleep(2.5)` (line 212) | ~5–8 s per poll |
| `goto()` | classify + `time.sleep(3)` (line 236) | ~5.5 s per iteration, 40 max |
| `map_select()` | screenshot (2 s settle) + panel OCR + 1.2 s sleep (lines 245–255) | ~3.5 s/step, 24 steps max ≈ 84 s |

The classifier has three concrete bugs. The problem is not only noisy OCR:

1. `map_select` reads the wrong rectangle. It OCRs `(980,120,300,480)`
   (`nioh_nav.py:248`), and that area is map *terrain*. The text STARTING POINT
   is at ≈(497,198)–(627,214) in `shots/a-lan-starting-point.png`. A 130×18 crop
   there OCRs exactly (`tesseract --psm 7` → `STARTING POINT.`). The function can
   therefore never match the string it looks for.
2. The title menu is misclassified. The font is unreadable and the fallback
   contradicts itself. `RULES` wants `load game`/`new game`
   (`nioh_nav.py:160–170`). On `shots/a-lan-main-menu.png`, a 4× upscale plus a
   threshold still OCRs `CINTINA` and `SYSTIM` (reproduced today). The
   `world_map` fallback at `nioh_nav.py:188–193` requires
   `"region" in low and "level" not in low`. The same screen's footer says
   “Level up, make offerings…”, so the condition can never be true on the real
   map footer.
3. `goto()` taps Return at `title_menu` blindly (`nioh_nav.py:230`). In
   `shots/a-lan-main-menu.png` the highlighted row is NEW GAME. In
   `shots/a-recovery-title-menu.png` it is CONTINUE. A blind Return starts a new
   game half the time. The code must read the selected row and must not assume
   it.

---

## 2. Option A: state oracle from game memory (recommended, do first)

Read-only `ReadProcessMemory` polling is already proven in this rig.
`$RIG/tools-tmp/admission-poll3.c/.exe` samples about 30 fields at 10 ms with no
debugger and no writes. `docs/FINDINGS.md:1068–1096` is built on its output.

### 2.1 Addresses that already answer the co-op questions

The global base is fixed at `0x140000000`. The poller validates MZ and the PE
timestamp `0x6307A81B` before it reads. All offsets below come from two sources:
the existing pollers and the disassembly in
`~/.cache/nioh3p/out/text.dis` (`objdump`, VMA = `0x140000000`).

| what | expression | evidence |
|---|---|---|
| game flow object `g` | `g = *(0x1418715E0)` | 1,565 references in `text.dis`; `UI::EnableMenuOpen` stores through it |
| **menu open** (byte) | `*(g+0xE5)` | writers `0x1408AABB4`, `0x14094C3A4` inside `UI::EnableMenuOpen` (0x1408AAB70 / 0x14094C360). Confirm the read side live; other objects also have a `+0xE5` byte (`0x140A9D9E1`) |
| online mode (u32) | `*(g+0x5B0)` | `Online::IsPlayerCoop` 0x14094A5B0 = `mode==1`; `Online::IsPlayerTokoyoCoop` 0x14094A700 and `Mission::IsCooperating` 0x140948110 test `==2`; the admission poller logs it as `p5b0` |
| **loader phase** (u32) | `*(g+0x5D0)` | observed `1` while loading (`docs/FINDINGS.md:1165`); mission-only gates compare it to `0xC` (`0x14086A5D7`, `0x140D5E036`, `0x140D794BE`, `0x140D7B72D`) ⇒ **[INFERENCE]** `0xC` = in-game/loaded |
| coop latch (byte) | `*(g+0x660)` | compared by `Mission::IsCooperating` 0x140948110 |
| mission manager | `*(0x14189F400)` | 5,512 refs; `Mission::YashiroOpenMenu` dereferences it. **[INFERENCE]** null outside a mission — cheap "am I in a mission" test to confirm live |
| session root | `0x141790A70` | proxy comment `src/proxy/nioh3p_proxy.c:2419`; poller |
| visitor allowance | `*(root+0x39C)` | `docs/FINDINGS.md:1068–1096` |
| **network/session state** | `*(root+0x3C0)` | poller `st3c0`; admission "state 9" (`docs/FINDINGS.md:1174`) **[INFERENCE]** that `0x3C0` is that state |
| slot *i* record | `root + i*0xA8 + {0xE8, 0x104, 0x10B}` | poller; connected member `e8=2`, flags `0x79/0x7d`, admitted `f=04` (`docs/FINDINGS.md:1072,1112,1187`) |
| Steam session singleton | `*(0x142C96250)`; `+0x198` count, `+0x1A0/+0x1A8` arrays, `+0x1B0/+0x1B4` indexes | `docs/RE_SESSION_FLOW.md:141–179`; `join-state.c` |
| admission inputs | `*(0x14191DE34)` mode, `*(0x14191DE38)` online flag | `docs/FINDINGS.md:1072–1079` |

These states map onto the ones the driver needs:

* boot/idle (title or logo): the root is untouched, so `root+0x39C==1`,
  `0x14191DE34==0`, `0x14191DE38==0` and `root+0x3C0==0` (values recorded at
  boot in `docs/FINDINGS.md:1072`). **[INFERENCE]** These values are valid for
  title and logo.
* loading: `g+0x5D0 == 1` (observed). Poll at 20 ms. This check alone removes
  the "is it loaded yet?" screenshot loop.
* in mission: `*(0x14189F400) != 0` and `g+0x5D0 == 0xC` (to make sure live).
* searching to joined: `root+0x3C0` changes (admission 9) and the slot table
  converges (`e8=2`, `f=0x04`, later `0x79/0x7d`). The existing 10 ms poller
  prints exactly these values. They are the ground truth for "did the join
  work".
* menu open: `g+0xE5`, which `UI::EnableMenuOpen` sets. Make sure that this
  byte works live before you rely on it. The game itself writes this byte at
  menu open.

### 2.2 What memory does *not* tell you

Memory does not give the screen identity or the cursor row. That covers title,
map, torii and search, and which row is highlighted. No script binding exposes
a screen id. The script API is the *mission* VM, and
`analysis/script_api.txt` has no title, map or load-screen functions. No
documented global holds one either. Two ways get it:

1. Vision. Option D below already solves it at low cost.
2. Field discovery, 20 minutes, once. Run the 10 ms poller against a manually
   navigated session. Dump `g[0x000..0x800]` and `0x14189F400` at every screen
   change, with a screenshot as the label. Pick the fields that step at each
   transition. **[INFERENCE]** The flow object almost certainly carries a
   front-end scene id or state id, because +0x5B0, +0x5D0 and +0xE5 already
   live there.

### 2.3 Delivery mechanics (the permission detail that matters)

`/proc/<pid>/mem` under `kernel.yama.ptrace_scope=1` (this machine) allows only
a descendant to attach. Measured today:

* A child process spawned by the reader: `open("/proc/pid/mem")` returns ok.
* A sibling or reparented process: `PermissionError(13, EACCES)`.

Consequences:

* If `coop.py` launches the slot itself (`run.sh X lan` as a child), every
  `nioh.exe` in the chain is a descendant. A pure-Python poller then works with
  no extra process and no Windows helper. Reading about 40 bytes every 10 ms
  costs negligible CPU.
* If something else launched the game, use the existing `admission-poll3.exe`
  (`proton runinprefix`, same prefix/HOME). It is already proven, has about
  10 ms granularity and prints a line only on change. `coop.py` must support
  both backends.
* Do not run `sysctl ptrace_scope=0`. It is not needed, and it weakens the
  machine.

**[INFERENCE]** Wine or Proton can clear the dumpable flag. If a descendant
still gets EACCES, fall back to the Windows poller. The design does not change.

Use 10 ms polling for load and join phases. Use 50 ms polling while a menu is
stable. The risk to the rig and the real install is none. The reads are
read-only (`PROCESS_VM_READ` and `/proc/pid/mem` reads), with no writes, no
debugger and no DLL change. The real Steam install is never in the path.

---

## 3. Option B: in-process command channel (assessed, not recommended)

### 3.1 New artifact: the script-binding table maps names to addresses

The merged decrypted image `~/.cache/nioh3p/dumps/nioh3p_image_merged.bin`
holds the engine's script binding tables as `(name_ptr, func_ptr)` pairs in
`.rdata`. Three contiguous runs hold them: `0x119E400`, `0x11F0AA0–0x11F3140`
and `0x120CF90–0x120F8C0`. Extraction yields 1,462 pairs and 1,384 unique
names:

```python
d = open(dump, 'rb').read(); base = 0x140000000
for off in range(0, len(d) - 16, 8):
    a, b = struct.unpack_from('<QQ', d, off)
    raw = d[a-base:a-base+130].split(b'\0')[0] if base <= a < base+len(d) else b''
    if b'::' in raw and base + 0x1000 <= b < base + len(d):
        print(raw.decode(), hex(b))
```

Validation: the extraction reproduces the address that `docs/FINDINGS.md:1167`
already relies on, `Load::IsLoadEnd` = 0x14089BF90. The table below shows
selected results. Appendix A has the full table.

| binding | address |
|---|---|
| `Mission::YashiroOpenMenu` (shrine menu) | 0x140949620 |
| `UI::EnableMenuOpen` / `UI::IsMenuOpen` | 0x1408AAB70, 0x14094C360 / 0x1408AB120 |
| `Online::IsPlayerHost` / `IsPlayerCoop` / `IsOnlinePlay` | 0x14094A690 / 0x14094A5B0 / 0x14094A320 |
| `Mission::IsCooperating` | 0x140948110 |
| `Online::GetExistPlayerNum` | 0x1408A1580 |
| `Load::IsLoadEnd` | 0x14089BF90 |

This also gives a safe way to re-derive any state predicate read-only. The
function bodies are in `text.dis`, so you can see which global each one tests
without calling anything. The analysis above did this for `IsPlayerHost` and
`IsCooperating`.

### 3.2 Why calling them from the proxy is a bad idea

Every binding function has the same generated prologue:

```
mov  %rcx,%rdx                 ; original args
lea  0x30(%rsp),%rcx
call 0x140DDF4E0               ; script VM argument marshaler
...
call 0x140DDFD00 / 0x140DDFD50 ; push result into the VM frame
call 0x1410F58B0               ; stack cookie
```

`rcx` is not a plain C++ `this` or value. It is a script-VM argument frame. A
call to `Mission::YashiroOpenMenu` from a proxy worker thread with a fabricated
`rcx` writes through that marshaler and corrupts the VM stack. The function
also dereferences the mission manager (`0x14189F400`) and a +0x48 member, and
it performs a task dispatch. A crash is the expected outcome.

The menu actions are also not these functions. The task layer runs
`0x140D71E00` (host), `0x140D73240` (join from a search result),
`0x140D73380`/`0x140D73480` (join by lobby id) and `0x140609D50` (search), as
`docs/RE_SESSION_FLOW.md:701–714` records. They are UI-task internals with no
external callers. They need constructed descriptors and scheduler context. The
co-op transport in this game is EOS (`0x1405BF3F0`), and the Steam class cannot
join from an invite (`docs/RE_SESSION_FLOW.md:717–750`).

Verdict: read-only memory (A) gives the same state information safely.
In-process action calls take days of work and carry high crash risk. No
evidence shows that the descriptor and task contracts can be satisfied. Do not
do it.

### 3.3 The in-process channel that *is* worth building (B1)

Input is the fragile part of the current rig. The rig shows `EI_EVENT_SYNC` and
libei warnings, and `XInputExtension` failures in
`tools-tmp/c-xinput-failed-steam485510.log`. `nioh.exe` imports only
`DirectInput8Create` from dinput8. The proxy already forwards DirectInput
(`README.md:36`). A rig-only research build can do two things:

* Wrap `IDirectInputDevice8::GetDeviceState/GetDeviceData` and OR synthetic key
  bits from a trigger file. This gives frame-exact presses and holds with no
  X11, no gamescope input path and no uinput.
* Write a 1 Hz line with the same state fields as (A) to a file. The driver
  then has one source of truth.

Gate it behind a new ini key and keep it out of `dist/nioh3p-player.zip`. Effort
is 1–2 d. Risk is medium, because a bug affects every launch. Do it only if
XTest or uinput still misbehaves.

---

## 4. Option C: save-state shortcut (per-role save templates)

### 4.1 Where the saves actually are

The rig's saves are not in Steam `userdata`. Those `485510/ac/WinMyDocuments`
trees are empty. The saves live in each prefix's Documents:

| slot | path (under `~/Projects/gaming/nioh-mp-mod-rig`) | account dir |
|---|---|---|
| a | `home-a/.local/share/Steam/steamapps/compatdata/485510/pfx/drive_c/users/steamuser/Documents/KoeiTecmo/NIOH/Savedata/` | account-specific |
| b | `home-b/.../compatdata/485510/pfx/.../KoeiTecmo/NIOH/Savedata/` | account-specific |
| c | `compat-c/pfx/drive_c/users/steamuser/Documents/KoeiTecmo/NIOH/Savedata/` | account-specific |

Each account dir holds `SAVEDATA00/SAVEDATA.BIN`, the character save
(2,043,288 B), and `SYSTEMSAVEDATA00/SAVEDATA.BIN` (9,824 B). Both were
written at 2026-10-21 19:02. The dir also holds a 51-byte
`steam_autocloud.vdf`. A pristine pre-test copy already exists at
`$RIG/save-b-bak/Savedata/` (dated 2026-10-18). Steam Cloud must stay off.

### 4.2 What it buys

The save encodes the resume point that CONTINUE loads. **[INFERENCE]** This
comes from the observed recipe. A save taken in-mission at the Wreathed in
Flame starting shrine resumes the host at that shrine, with no 300 ms `w` walk.
A save taken on the world map resumes a visitor on the map with the Starting
Point marker current, with no map wandering. That removes 10–30 s and one class
of "where did the character land" failures per launch. It does not remove the
title, CONTINUE, the lore screen, loading or any menu step.

### 4.3 Procedure

1. Do this once per role and account. Play to the desired spot by hand. Quit
   through the game's menu so the save is complete. Then, with the game
   stopped, copy the two `SAVEDATA.BIN` files into
   `$RIG/saves/<slot>-<role>/{SAVEDATA00,SYSTEMSAVEDATA00}/` with
   `cp --reflink=auto`.
2. Before every launch, with the game stopped, copy the template back over the
   account dir. Never restore while the game runs. Nioh autosaves at shrines
   and on quit, so a live copy can be half-written, and a restore can be
   overwritten mid-session.
3. Make sure that the size and mtime are correct. The game rejects a bad save
   with its own error dialog, so keep the previous generation under
   `$RIG/saves/<slot>-prev/`.

Risk is low. The operation copies bytes of the same account's own saves, with
no decryption and no checksum edits. The SteamID stays the same, so the
savedata decryption tool's `-sid` transfer is not needed. That tool exists if a
save must ever move between accounts (`docs/RIG.md:194–200`). Storage is
negligible at 2 MB per role, with reflinks.

---

## 5. Option D: faster vision (recommended, with A)

### 5.1 Measured costs of each stage (today, 1280×720)

| stage | cost | note |
|---|---|---|
| `gamescopectl screenshot` | **62–71 ms** | request returns in 1–4 ms; the compositor writes the PNG asynchronously. Poll for the IEND trailer instead of sleeping (the sampled file was IEND-complete when it appeared). |
| `magick … -resize 640x360! -colorspace Gray gray:-` | **43 ms** | one decode feeds all crops |
| full-frame 32×18 thumbnail | 82 ms | PNG decode dominates |
| fixed-crop NCC in Python (200×22 bytes) | **< 1 ms** | pure Python, no numpy needed |
| row "bright fraction" (`-threshold 55% -format %[fx:mean]`) | ~10 ms/row | or µs from the decoded frame |
| full OCR (preprocess+tesseract) | 0.85 s | current path |

Per-check budget: about 110–150 ms end to end (screenshot, decode and one or
two crops). That is 25–45× faster than `classify()` and about 50× faster than a
`wait_state()` poll. If a step can be verified from memory (A), it needs no
screenshot at all.

### 5.2 Proven fingerprints and thresholds (tested on the real shots)

All coordinates are for 1280×720, with the origin at top-left.

Title menu (rows are fixed: NEW GAME, CONTINUE, LOAD GAME, SYSTEM):

| row | box | bright-fraction in `a-lan-main-menu.png` | in `a-recovery-title-menu.png` |
|---|---|---|---|
| NEW GAME | 190×22+148+450 | **0.105** | 0.000 |
| CONTINUE | 190×22+148+477 | 0.000 | **0.139** |
| LOAD GAME | 190×22+148+511 | 0.000 | 0.000 |
| SYSTEM | 190×22+148+543 | 0.000 | 0.000 |

* Exactly one row is bright, so the selected row is machine-readable. The two
  captures differ (NEW GAME vs CONTINUE highlighted). That is why a blind
  Return is wrong.
* Identity across captures: unselected rows match at NCC 0.972 / 0.981
  (LOAD GAME / SYSTEM). The different-selected rows score 0.146 / 0.231. Use
  an unselected row as the "this is the title menu" template. Use the
  bright-fraction test for the cursor.
* A whole-menu crop (200×120+140+445) is unstable, with NCC 0.34 between the
  two title captures. A fish swims across the background. Crop the text only.

World map (`a-lan-starting-point.png`): the left node list is fixed. Boxes are
≈x 90–420, with row pitch ≈64 px: SHRINE y≈165, BLACKSMITH y≈232, DOJO y≈296,
TORII GATE y≈360, HIDDEN TEAHOUSE y≈424, Storehouse y≈490. The selected row
has a cyan border. Counting pixels with `B>140 and B>R+15` in the six bands
gives 19.4 % for the selected SHRINE row and 0.0 % for every other band. Node
order is constant, so Torii Gate is always 3×Down from SHRINE. No OCR and no
search are needed. The label STARTING POINT is at ≈(497,198) 130×18, and
`--psm 7` can OCR it if a text check is ever wanted.

Torii menu (`a-torii-menu.png`): rows YOKAI REALM y≈200, RANDOM ENCOUNTERS
y≈254, BATTLE y≈310. Bright-fraction is 0.399 / 0.089 / 0.006, so the same
method works. A 140×18 strip of the "Random Encounters" row matched at NCC
1.000 on two independent captures of this screen. On map, search and title
shots it scored 0.18–0.26, so a threshold of 0.85 is safe.

Search screen (`a-search2.png`): the Search button is at ≈(434..846,
455..505). Rows are Difficulty y≈330, Friends y≈375 and Secret Word y≈415. The
ONLINE badge at ≈(950..1200, 45..100) is common to map, torii and search. It is
a cheap pre-filter for "we are in the online front-end".

### 5.3 Implementation notes

* Use one `magick` decode to 640×360 gray, then do every crop and sum in
  Python. Pure-Python NCC over a few thousand bytes takes under 1 ms, so
  `numpy` and OpenCV are not needed. Neither is installed:
  `python3 -c "import numpy, cv2"` fails.
* Calibrate once per slot. Identical copies need one calibration. Store boxes
  and templates as JSON in `tools/rig/`, and capture the templates again when
  the game's resolution changes.
* Never build a fingerprint from an animated region, such as the title
  background, map terrain or search spinner. Crop text and rows only. That is
  the difference between NCC 1.000 and 0.34.
* Fail-safe rule: if the bright-fraction test finds 0 or 2 or more selected
  rows, do not tap Return. Capture a debug screenshot and treat the screen as
  unknown.

Risk to the rig and the real install is none. The tool uses `gamescopectl`
screenshots and local image processing only. Nothing is written into the game
or its prefix.

---

## 6. Option E: launch arguments, movies, intros

### 6.1 What exists

* `movie/` holds 294 `.wmv` files, 3.53 GB. `EV0001.wmv` is 113,925,831 B,
  WMV3, 1920×1080, 108.975 s. `EV0001JP.wmv` is the JP variant (113,109,831 B).
  The exe's resource table lists `movie/EV0001.wmv` first, then `EV0001JP`,
  then `EV0101`. Each entry is `RTM_ID_*`, file name and 40-hex hash.
  **[INFERENCE]** EV0001 is the 109 s opening cinematic.
* No skip option and no launch option exists. The image contains no
  `skip*movie`, `no*movie`, `nomovie` or `disablemovie` strings. The game imports
  only ole32 `CoCreateInstance` for playback, with no wmvcore or DirectShow
  import. The rig starts `nioh.exe` directly, so the launcher is already
  bypassed.
* `Documents/KoeiTecmo/NIOH/nioh.ini` (45 B, dated 2026-07-03) contains
  `[MOVIE] bUseRecommendedCodec=1 / nUseDXVA=1`. Those literal keys are in
  neither `nioh.exe` nor `nioh_launcher.exe`. Which component consumes them is
  unverified. It can be the launcher's encoded data.
* The crash is at 0x14052F321: `mov (%rcx),%r8` with `rcx = [rsi+0x48]`
  (`text.dis`). It is an unchecked null COM interface. The preceding
  `call *0x50(%rax)` on `[rsi+0x40]` keeps its result in `edi`. The code checks
  that result only after the second call. A failed graph or decoder creation
  therefore runs straight into the null dereference. That is a
  playback-creation failure under Wine, not a key-handling bug.

### 6.2 Test plan (rig copies only, zero risk to the real install)

The game copies in `$RIG/lan-{a,b,c}` are reflinked clones, so this test is
safe:

1. Reflink-copy `lan-c` aside once, for example as `lan-c-movies-bak`. Do not
   use `rm`.
2. Rename `EV0001.wmv` and `EV0001JP.wmv` in `lan-c/movie/` only, with `mv` to
   `*.wmv.disabled`.
3. Cold-boot slot C. Outcome (a): the movie is skipped, so 109 s is saved and
   today's crash class is gone. Outcome (b): the game shows an error or a black
   screen, so restore the names. The RTM table carries a hash per file, so (b)
   is a real possibility.
4. For the orthogonal experiment, set `[MOVIE] nUseDXVA=0` in that prefix's
   `nioh.ini` and cold-boot. If the crash stops, keep it. If not, revert.
5. Do not touch `~/.local/share/Steam` or the real Documents.

Expected: −109 s per boot (plus the movie's own load) and removal of the one
crash seen today. Effort is 1 h. Risk is low because the copies are armed only.
Uncertainty is medium because the game can insist on the file.

---

## 7. Option F: `rig/coop.py` (the driver)

### 7.1 Shape

```
coop.py
├── Slot(slot)        # one process per slot
│   ├── launcher      # starts Xvfb :9/:10, client, `run.sh <slot> lan` (so we are its ancestor)
│   ├── state         # memory oracle: 10 ms sample → dataclass (phase, mode, menu, root, slots)
│   ├── vision        # screenshot (70 ms) + decode (43 ms) + fingerprints (<1 ms)
│   ├── input         # XTest now; uinput gamepad + DirectInput later
│   └── script        # named steps: press/hold/wait_for(predicate)/verify(screen)
├── Orchestrator      # host/visitor roles, ordering, health, SIGSTOP bursts
└── Log               # one JSONL line per transition: t, slot, step, state, screenshot path
```

### 7.2 Step contract

Every step is `{do, expect, timeout, retry}`:

* `do`: a short blind key sequence. Taps are 80 ms apart, so 4 taps take about
  0.6 s.
* `expect`: a predicate over the state oracle first, and vision second (`!= 0`,
  `phase==1`, `e8==2`, selected row index, screen template).
* `verify` runs after the step. On timeout, send `BackSpace` (verified: it
  backs out of menus, `docs/RIG.md:236`), re-read the screen and retry the step
  at most twice.
* `timeout` budgets: title 30 s, load 120 s (the 3-instance iGPU stall can take
  300 s, see below), menu 5 s, join 180 s.
* After a third failure: take a screenshot, dump the state and stop. Never
  continue blind.

Deterministic sequences replace most vision:

* title: read the selected row. Press `Down` until CONTINUE is selected
  (max 3), make sure of the row, then press Return.
* map: `Down ×3` (SHRINE→TORII GATE) plus Return. Make sure that the torii
  screen appeared.
* torii: read the highlight, `Down ×1` (Yokai Realm → Random Encounters), read
  again, Return. Then use the screen's own `Down` count plus Return.
* shrine: hold `E` for 1.7 s. Make sure of `g+0xE5` (menu open) or the
  shrine-menu template.
* The map's fixed node list is why `map_select` and its 24-step OCR spiral can
  be deleted entirely.

### 7.3 Three-instance realities the driver must encode

* Load is GPU-bound, not protocol-bound. The second visitor's loader sits in
  phase 1 for 280–300 s because three games share the iGPU. `SIGSTOP` on the
  other visitor in 5 s bursts cut a load to 33 s (`docs/FINDINGS.md:1170–1190`).
  The driver must implement this. When `phase==1` for the newcomer and the
  other visitor is in-mission, pause that other visitor's `nioh.exe` in 5 s
  bursts. Send `SIGCONT` at the end of every burst and before any other action.
  Never pause the host.
* Re-join cost: after a visitor leaves, the host's allowance stays 0 until
  co-op ends (`docs/FINDINGS.md:1200`). `$RIG/tools-tmp/budget-set.exe` already
  reopens it (documented in `docs/RIG.md:320`). It turns a ~60 s host menu
  replay into ~1 s. It is a diagnostic write, so gate it as a rig-only fast
  path.
* Warm sessions beat relaunches. Keep the host in the mission between tests and
  re-attach the visitors. A cold launch costs the boot plus a 20 s mission load
  before any menu work. A second visitor can add 33–300 s.
* Order: the host first (pray, Summon Visitor, Make Offering), then the
  visitors (Starting Point, Torii Gate, Random Encounters, Search).

### 7.4 Expected effect

| phase | now | after A+D+F |
|---|---|---|
| state check | 3.4–8 s | 0.00 s (memory) / 0.11–0.15 s (vision) |
| map navigation | up to 84 s, OCR-dependent | ~1 s, deterministic |
| menu step verify | ~5 s | ~0.12 s |
| join detection | OCR + 2.5 s sleeps | 10 ms slot-table poll |
| overall host+2 visitors (excluding game load) | minutes | ~5–10 s |
| re-join after a departure | ~60 s + menu replay | ~1–3 s (warm + budget-set) |

The remaining floor is game load: about 20 s for a first visitor and 33–300 s
for a second on this single iGPU.

---

## 8. Ranking and recommended plan

Ranked by impact divided by risk:

1. F+A+D together: memory oracle, fingerprints and deterministic `coop.py`.
   This removes all OCR and all fixed sleeps, and every wait becomes an event.
2. A alone, if only one thing is done now. The 10 ms poller already answers
   "loading", "joined" and "allowance", and it is proven.
3. C: save templates. It is cheap and removes the walk and the map wandering.
4. E: the movie experiment. It is cheap and saves 109 s per boot, and it
   removes the crash.
5. Warm-session rejoin with `budget-set.exe`: the biggest per-test win, but a
   memory write, so rig-only.
6. B1 DirectInput injection: only if input remains the flaky part.
7. B2 script-binding calls: not recommended.

### Recommended 3-step plan

Step 1 takes ½–1 day. Add `tools/rig/state.py` with a dual backend: `/proc`
when `coop.py` launched the slot, else `admission-poll3.exe`. Add
`tools/rig/vision.py`, which decodes once and reads title rows, map rows, torii
rows and the search button with the measured boxes in §5.2. Add
`tools/rig/coop.py` with the step contract. Replace `classify`, `map_select`
and `goto` in `nioh_nav.py`. Delete the 2 s settle and the 2.5 s and 3 s
sleeps. Acceptance: a full host and visitor setup with zero OCR, and every
transition logged with its predicate and timing. A deliberate wrong-row start
must be detected and corrected. The title path must never press Return on NEW
GAME.

Step 2 takes ½ day. Add save templates per role and run the launch movie
experiment. Create `$RIG/saves/{b-host,a-visitor,c-visitor}/`. Add
`--restore-saves` to `coop.py` for a stopped game only. Run the EV0001 rename
test and the `nUseDXVA=0` test on slot C. Acceptance: the host and the visitors
CONTINUE to the intended spot on 3 of 3 boots, and slot C boots without the
DirectShow crash.

Step 3 is optional and takes 1 day. If input still misses or desyncs, add the
DirectInput wrapper (B1) as a research-build INI option. Otherwise automate the
warm-session rejoin path with budget-set and SIGSTOP bursts, and measure a full
3-player re-test cycle end to end.

---

## Appendix A: script-binding addresses (extracted from the merged dump)

`Mission::YashiroOpenMenu` 0x140949620 · `Mission::IsCooperating` 0x140948110 ·
`Mission::YashiroAutoSave` 0x140949590 · `Mission::IsYashiroActive` 0x1409488A0 ·
`UI::IsMenuOpen` 0x1408AB120 · `UI::EnableMenuOpen` 0x1408AAB70 / 0x14094C360 ·
`UI::LoadStart` 0x1408AB550 · `UI::LoadEnd` 0x1408AB4F0 ·
`Online::IsOnlinePlay` 0x14094A320 · `Online::IsPlayerHost` 0x14094A690 ·
`Online::IsPlayerGuest` 0x14094A620 · `Online::IsPlayerCoop` 0x14094A5B0 ·
`Online::IsPlayerCoop1/2` 0x14094A490 / 0x14094A520 ·
`Online::IsPlayerTokoyoCoop` 0x14094A700 · `Online::GetExistPlayerNum` 0x1408A1580 ·
`Check::IsCooperating` 0x1406F1530 · `Load::IsLoadEnd` 0x14089BF90 ·
`Character::GetSummonSlotNum` 0x14088D790 · `Check::GetSummonSlotNum` 0x1406F1330 ·
`Pause::SetFlag` 0x1408A2160.

Session/UI task layer (not script bindings, see `docs/RE_SESSION_FLOW.md`):
host 0x140D71E00 · join-from-search 0x140D73240 · join-by-lobby-id
0x140D73380 (Steam) / 0x140D73480 (EOS) · search 0x140609D50.

## Appendix B: measured one-liners (reproduce)

```sh
# screenshot cost (needs a live gamescope; measured with vkcube, no game)
XDG_RUNTIME_DIR=/tmp/gsrun GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
  gamescopectl screenshot /tmp/x.png     # ~62-71 ms until IEND

# decode cost
magick shot.png -resize 640x360! -colorspace Gray -depth 8 gray:- > /tmp/f.raw  # 43 ms

# selected title row (exactly one row is non-zero)
magick shot.png -crop 190x22+148+450 +repage -colorspace Gray \
  -threshold 55% -format '%[fx:mean]' info:

# map: which left-panel row has the cyan highlight (B>140, B>R+15)
# torii: same per-row test at y=200/254/310
```

## Appendix C: files/addresses cited

* `tools/rig/nioh_nav.py` 108–113 (2 s settle), 160–170 (RULES), 175–199
  (classify), 212 (2.5 s poll), 230 (blind Return), 243–256 (map_select crop).
* `docs/FINDINGS.md` 1068–1096 (admission table), 1096–1126 (3-player run),
  1170–1190 (load stall), 1165–1167 (`phase`, `Load::IsLoadEnd`), 1200 (allowance
  after departure). `docs/RIG.md` 194–200 (savedata tool), 236 (BackSpace),
  320 (tools-tmp pollers). `docs/RE_SESSION_FLOW.md` 113–179 (session struct),
  701–714 (UI task entry points), 717–750 (absences).
* `src/proxy/nioh3p_proxy.c` 2419 (session root), 2250–2270 (trace hooks),
  2555 (mode line).
* Disassembly `~/.cache/nioh3p/out/text.dis` (VMA 0x140000000), merged image
  `~/.cache/nioh3p/dumps/nioh3p_image_merged.bin`.
