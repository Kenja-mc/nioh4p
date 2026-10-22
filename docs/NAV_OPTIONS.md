# Faster menu / game navigation for the nioh3p rig — ranked options

Scope: how to get host + visitors from a cold client to "in co-op" (and back to a
re-testable state) without the current screenshot+OCR+fixed-sleep loop.
All measurements were taken read-only on this machine on 2026-10-21; no game was
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

Recommended plan (3 steps): **1)** ship `coop.py` = memory oracle + fingerprints +
deterministic sequences (A+D+F); **2)** add per-role save templates (C) and test the
movie workaround on slot C (E); **3)** only if input still flakes, add B1.

---

## 1. Baseline: what the current loop actually costs

`tools/rig/nioh_nav.py`:

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

Three concrete bugs in the classifier (not just "OCR is noisy"):

1. **`map_select` reads the wrong rectangle.** It OCRs `(980,120,300,480)`
   (`nioh_nav.py:248`) — that is map *terrain*. The text **STARTING POINT** is at
   ≈(497,198)–(627,214) in `shots/a-lan-starting-point.png`; a 130×18 crop there
   OCRs exactly (`tesseract --psm 7` → `STARTING POINT.`). It can therefore never
   match the string it is looking for.
2. **title is misclassified because the font is unreadable and the fallback is
   contradictory.** `RULES` wants `load game`/`new game` (`nioh_nav.py:160–170`),
   but on `shots/a-lan-main-menu.png` a 4× upscale + threshold still OCRs
   `CINTINA`, `SYSTIM` (reproduced today). The `world_map` fallback at
   `nioh_nav.py:188–193` requires `"region" in low and "level" not in low`, and
   the same screen's footer says “Level up, make offerings…” — so the condition
   can never be true on the real map footer.
3. **`goto()` taps Return at `title_menu` blindly (`nioh_nav.py:230`).** In
   `shots/a-lan-main-menu.png` the highlighted row is **NEW GAME**; in
   `shots/a-recovery-title-menu.png` it is **CONTINUE**. A blind Return starts a
   new game half the time. The selected row must be read, not assumed.

---

## 2. Option A — state oracle from game memory (recommended, do first)

Read-only `ReadProcessMemory` polling is already proven in this rig:
`$RIG/tools-tmp/admission-poll3.c/.exe` samples ~30 fields at 10 ms with no
debugger and no writes, and `docs/FINDINGS.md:1068–1096` is built on its output.

### 2.1 Addresses that already answer the co-op questions

Global base is fixed at `0x140000000` (the poller validates MZ + PE timestamp
`0x6307A81B` before reading). All offsets below are **verified** either by the
existing pollers or by the disassembly in
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

Mapping to the states the driver needs:

* **boot/idle (title or logo):** root untouched — `root+0x39C==1`, `0x14191DE34==0`,
  `0x14191DE38==0`, `root+0x3C0==0` (values recorded at boot in
  `docs/FINDINGS.md:1072`). **[INFERENCE]** valid for title/logo.
* **loading:** `g+0x5D0 == 1` (observed). Poll at 20 ms; this alone removes the
  "is it loaded yet?" screenshot loop.
* **in mission:** `*(0x14189F400) != 0` and `g+0x5D0 == 0xC` (to confirm live).
* **searching → joined:** `root+0x3C0` transitions (admission 9) and the slot table
  converges (`e8=2`, `f=0x04`, later `0x79/0x7d`). This is exactly what the
  existing 10 ms poller prints, and it is the ground truth for "did the join work".
* **menu open:** `g+0xE5` (set by `UI::EnableMenuOpen`). Confirm live before relying
  on it; it is a byte the game itself writes at menu open.

### 2.2 What memory does *not* tell you

The **screen identity and cursor row** (title / map / torii / search, and which
row is highlighted). No script binding exposes a screen id (the script API is the
*mission* VM: `analysis/script_api.txt` has no title/map/load-screen functions),
and no documented global holds one. Two ways to get it:

1. **Vision** — Option D below already solves it cheaply.
2. **Field discovery, 20 min, once:** run the 10 ms poller against a manually
   navigated session and dump `g[0x000..0x800]` + `0x14189F400` at every screen
   change, labelling with a screenshot. Pick the field(s) that step at each
   transition. **[INFERENCE]** the flow object almost certainly carries a
   front-end scene/state id, because +0x5B0/+0x5D0/+0xE5 already live there.

### 2.3 Delivery mechanics (the permission detail that matters)

`/proc/<pid>/mem` under `kernel.yama.ptrace_scope=1` (this machine) allows only a
**descendant** to attach. Measured today:

* child process spawned by the reader → `open("/proc/pid/mem")` **ok**;
* sibling/reparented process → `PermissionError(13, EACCES)`.

Consequences:

* If **`coop.py` launches the slot itself** (`run.sh X lan` as a child), every
  `nioh.exe` in the chain is a descendant → a pure-Python poller works with no
  extra process and no Windows helper. Reading ~40 bytes every 10 ms is
  negligible CPU.
* If the game was launched by something else, use the existing
  `admission-poll3.exe` (`proton runinprefix`, same prefix/HOME) — already proven,
  ~10 ms granularity, prints a line only on change. `coop.py` must support both
  backends.
* Do **not** `sysctl ptrace_scope=0`; not needed and it weakens the machine.

**[INFERENCE]** Wine/Proton may clear the dumpable flag; if a descendant still
gets EACCES, fall back to the Windows poller (no design change).

Polling frequency: 10 ms for load/join phases, 50 ms while a menu is stable.
Risk to the rig/real install: **none** — read-only (`PROCESS_VM_READ` /
`/proc/pid/mem` reads), no writes, no debugger, no DLL change; the real Steam
install is never in the path.

---

## 3. Option B — in-process command channel (assessed, not recommended)

### 3.1 New artifact: the script-binding table maps names → addresses

The merged decrypted image `~/.cache/nioh3p/dumps/nioh3p_image_merged.bin`
contains the engine's script binding tables as `(name_ptr, func_ptr)` pairs in
`.rdata` (three contiguous runs: `0x119E400`, `0x11F0AA0–0x11F3140`,
`0x120CF90–0x120F8C0`). Extracting them yields **1,462 pairs / 1,384 unique
names**:

```python
d = open(dump, 'rb').read(); base = 0x140000000
for off in range(0, len(d) - 16, 8):
    a, b = struct.unpack_from('<QQ', d, off)
    raw = d[a-base:a-base+130].split(b'\0')[0] if base <= a < base+len(d) else b''
    if b'::' in raw and base + 0x1000 <= b < base + len(d):
        print(raw.decode(), hex(b))
```

Validation: it reproduces the address `docs/FINDINGS.md:1167` already relies on —
`Load::IsLoadEnd` = **0x14089BF90**. Selected results (full table in Appendix):

| binding | address |
|---|---|
| `Mission::YashiroOpenMenu` (shrine menu) | 0x140949620 |
| `UI::EnableMenuOpen` / `UI::IsMenuOpen` | 0x1408AAB70, 0x14094C360 / 0x1408AB120 |
| `Online::IsPlayerHost` / `IsPlayerCoop` / `IsOnlinePlay` | 0x14094A690 / 0x14094A5B0 / 0x14094A320 |
| `Mission::IsCooperating` | 0x140948110 |
| `Online::GetExistPlayerNum` | 0x1408A1580 |
| `Load::IsLoadEnd` | 0x14089BF90 |

This also gives a safe way to **re-derive** any state predicate read-only: the
functions' bodies are in `text.dis`, so you can see which global each one tests
(as done for `IsPlayerHost`/`IsCooperating` above) without calling anything.

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

`rcx` is not a plain C++ `this`/value — it is a script-VM argument frame. Calling
`Mission::YashiroOpenMenu` from a proxy worker thread with a fabricated `rcx`
writes through that marshaler and corrupts the VM stack; the function also
dereferences the mission manager (`0x14189F400`) and a +0x48 member and performs
a task dispatch. A crash is the expected outcome.

The menu *actions* are also not these functions: the task layer runs
`0x140D71E00` (host), `0x140D73240` (join from a search result),
`0x140D73380`/`0x140D73480` (join by lobby id), `0x140609D50` (search)
(`docs/RE_SESSION_FLOW.md:701–714`). They are UI-task internals with no external
callers, they need constructed descriptors plus scheduler context, and the co-op
transport in this game is **EOS** (`0x1405BF3F0`), while the Steam class cannot
join from an invite (`docs/RE_SESSION_FLOW.md:717–750`).

**Verdict:** read-only memory (A) gets the same state information safely.
In-process *action* calls: days of work, high crash risk, no evidence the
descriptor/task contracts can be satisfied. Do not.

### 3.3 The in-process channel that *is* worth building (B1)

Input is the fragile part of the current rig (`EI_EVENT_SYNC`/libei warnings,
`XInputExtension` failures in `tools-tmp/c-xinput-failed-steam485510.log`).
`nioh.exe` imports **only `DirectInput8Create`** from dinput8, and the proxy
already forwards DirectInput `README.md:36`. A rig-only research build can:

* wrap `IDirectInputDevice8::GetDeviceState/GetDeviceData` and OR synthetic key
  bits read from a trigger file, giving frame-exact presses/holds with no X11,
  no gamescope input path, no uinput; and
* write a 1 Hz line of the same state fields as (A) to a file, so the driver has
  one source of truth.

Gate it behind a new ini key and keep it out of `dist/nioh3p-player.zip`. Effort
1–2 d; risk medium (a bug affects every launch); do it only if XTest/uinput still
misbehaves.

---

## 4. Option C — save-state shortcut (per-role save templates)

### 4.1 Where the saves actually are

The rig's saves are **not** in Steam `userdata` (those `485510/ac/WinMyDocuments`
trees are empty); they live in each prefix's Documents:

| slot | path (under `~/nioh3p-test`) | account dir |
|---|---|---|
| a | `home-a/.local/share/Steam/steamapps/compatdata/485510/pfx/drive_c/users/steamuser/Documents/KoeiTecmo/NIOH/Savedata/` | `<accountid-private-1>` |
| b | `home-b/.../compatdata/485510/pfx/.../KoeiTecmo/NIOH/Savedata/` | `<accountid-private-2>` |
| c | `compat-c/pfx/drive_c/users/steamuser/Documents/KoeiTecmo/NIOH/Savedata/` | `63447459` |

Each account dir holds `SAVEDATA00/SAVEDATA.BIN` (**2,043,288 B**, the character
save) and `SYSTEMSAVEDATA00/SAVEDATA.BIN` (**9,824 B**), both written at
2026-10-21 19:02, plus a 51-byte `steam_autocloud.vdf`. A pristine pre-test copy
already exists at `~/nioh3p-test/save-b-bak/Savedata/<accountid-private-2>/` (dated 2026-10-18).
Steam Cloud must stay off (it is, per `docs/RIG.md:57`).

### 4.2 What it buys

The save encodes the resume point that CONTINUE loads. **[INFERENCE]** from the
observed recipe: a save taken in-mission at the Wreathed in Flame starting shrine
resumes the host at that shrine (no 300 ms `w` walk); a save taken on the world map
resumes a visitor on the map with the Starting Point marker current (no map
wandering). That removes 10–30 s and a class of "where did the character land"
failures per launch — but it does **not** remove the title, CONTINUE, the lore
screen, loading, or any menu step.

### 4.3 Procedure

1. Once per role and account: play to the desired spot by hand, quit through the
   game's menu (so the save is complete), then, with the game stopped,
   `cp --reflink=auto` the two `SAVEDATA.BIN` files into
   `$RIG/saves/<slot>-<role>/{SAVEDATA00,SYSTEMSAVEDATA00}/`.
2. Before every launch, with the game stopped, copy the template back over the
   account dir. Never restore while the game runs: Nioh autosaves at shrines and
   on quit, so a live copy can be half-written and a restore can be overwritten
   mid-session.
3. Verify by size+mtime; the game rejects a bad save with its own error dialog, so
   keep the previous generation under `$RIG/saves/<slot>-prev/`.

Risk: **low** — byte copies of the same account's own saves, no decryption, no
checksum edits (same SteamID, so the savedata decryption tool's `-sid` transfer is
not needed; it exists if a save must ever move between accounts —
`docs/RIG.md:194–200`). Storage is negligible (2 MB/role, reflinks).

---

## 5. Option D — faster vision (recommended, with A)

### 5.1 Measured costs of each stage (today, 1280×720)

| stage | cost | note |
|---|---|---|
| `gamescopectl screenshot` | **62–71 ms** | request returns in 1–4 ms; the compositor writes the PNG asynchronously. Poll for the IEND trailer instead of sleeping (the sampled file was IEND-complete when it appeared). |
| `magick … -resize 640x360! -colorspace Gray gray:-` | **43 ms** | one decode feeds all crops |
| full-frame 32×18 thumbnail | 82 ms | PNG decode dominates |
| fixed-crop NCC in Python (200×22 bytes) | **< 1 ms** | pure Python, no numpy needed |
| row "bright fraction" (`-threshold 55% -format %[fx:mean]`) | ~10 ms/row | or µs from the decoded frame |
| full OCR (preprocess+tesseract) | 0.85 s | current path |

**Per-check budget: ~110–150 ms end to end** (screenshot + decode + one or two
crops). That is **25–45× faster** than `classify()` and ~50× faster than a
`wait_state()` poll. If a step can be verified from memory (A), it needs **no
screenshot at all**.

### 5.2 Proven fingerprints and thresholds (tested on the real shots)

All coordinates are for 1280×720, origin top-left.

**Title menu** (rows are fixed: NEW GAME, CONTINUE, LOAD GAME, SYSTEM):

| row | box | bright-fraction in `a-lan-main-menu.png` | in `a-recovery-title-menu.png` |
|---|---|---|---|
| NEW GAME | 190×22+148+450 | **0.105** | 0.000 |
| CONTINUE | 190×22+148+477 | 0.000 | **0.139** |
| LOAD GAME | 190×22+148+511 | 0.000 | 0.000 |
| SYSTEM | 190×22+148+543 | 0.000 | 0.000 |

* Exactly one row is bright ⇒ the selected row is machine-readable. The two
  captures differ (NEW GAME vs CONTINUE highlighted), which is why a blind Return
  is wrong.
* Identity across captures: unselected rows match at NCC **0.972 / 0.981**
  (LOAD GAME / SYSTEM), while the different-selected rows score 0.146 / 0.231.
  Use an unselected row as the "this is the title menu" template, and the
  bright-fraction test for the cursor.
* A whole-menu crop (200×120+140+445) is unstable (NCC 0.34 between the two title
  captures) because a **fish swims across the background** — crop the text only.

**World map** (`a-lan-starting-point.png`): fixed left node list, boxes
≈x 90–420, row pitch ≈64 px: SHRINE y≈165, BLACKSMITH y≈232, DOJO y≈296,
TORII GATE y≈360, HIDDEN TEAHOUSE y≈424, Storehouse y≈490. The selected row has a
cyan border: counting pixels with `B>140 and B>R+15` in the six bands gives
**19.4 % for the selected SHRINE row and 0.0 % for every other band**. Node order
is constant ⇒ *Torii Gate is always 3×Down from SHRINE*; no OCR and no search are
needed. The label **STARTING POINT** is at ≈(497,198) 130×18 (OCR-able with
`--psm 7` if a text check is ever wanted).

**Torii menu** (`a-torii-menu.png`): rows YOKAI REALM y≈200, RANDOM ENCOUNTERS
y≈254, BATTLE y≈310; bright-fraction **0.399 / 0.089 / 0.006** ⇒ same method.
A 140×18 strip of the "Random Encounters" row matched at **NCC 1.000** on two
independent captures of this screen, and 0.18–0.26 on map/search/title shots ⇒
threshold 0.85 is safe.

**Search screen** (`a-search2.png`): Search button ≈(434..846, 455..505);
rows Difficulty y≈330, Friends y≈375, Secret Word y≈415. "ONLINE" badge
≈(950..1200, 45..100) is common to map/torii/search ⇒ useful as a cheap
"we are in the online front-end" pre-filter.

### 5.3 Implementation notes

* Use **one** `magick` decode to 640×360 gray and do every crop/sum in Python;
  pure-Python NCC over a few thousand bytes is < 1 ms, so `numpy`/OpenCV are not
  needed (neither is installed: `python3 -c "import numpy, cv2"` fails).
* Calibrate once per slot (identical copies ⇒ one calibration), store boxes +
  templates as JSON in `tools/rig/`, and re-capture templates when the game's
  resolution changes.
* Never build a fingerprint from an animated region (title background, map
  terrain, search spinner); crop text/rows only. That is the difference between
  NCC 1.000 and 0.34.
* Fail-safe rule: if the bright-fraction test finds **0 or 2+** selected rows, do
  not tap Return — capture a debug screenshot and treat the screen as unknown.

Risk to the rig/real install: **none** — `gamescopectl` screenshots and local
image processing only; nothing is written into the game or its prefix.

---

## 6. Option E — launch arguments, movies, intros

### 6.1 What exists

* `movie/` = **294 `.wmv`, 3.53 GB**. `EV0001.wmv` is 113,925,831 B, WMV3,
  1920×1080, **108.975 s**; `EV0001JP.wmv` is the JP variant (113,109,831 B).
  The exe's resource table lists `movie/EV0001.wmv` first (each entry is
  `RTM_ID_*`, file name, 40-hex hash), then `EV0001JP`, then `EV0101`.
  **[INFERENCE]** EV0001 is the 109 s opening cinematic.
* **No skip/launch option:** the image contains no `skip*movie`, `no*movie`,
  `nomovie`, `disablemovie` strings; the game imports only ole32
  `CoCreateInstance` for playback (no wmvcore/DirectShow import). The rig starts
  `nioh.exe` directly, so the launcher is already bypassed.
* `Documents/KoeiTecmo/NIOH/nioh.ini` (45 B, dated 2026-07-03) contains
  `[MOVIE] bUseRecommendedCodec=1 / nUseDXVA=1`. Those literal keys are in
  **neither** `nioh.exe` nor `nioh_launcher.exe`, so which component consumes
  them is **unverified** (it may be the launcher's encoded data).
* The crash is at **0x14052F321**: `mov (%rcx),%r8` with `rcx = [rsi+0x48]`
  (`text.dis`). It is an unchecked null COM interface: the preceding
  `call *0x50(%rax)` on `[rsi+0x40]` has its result kept in `edi` but is only
  checked **after** the second call, so a failed graph/decoder creation runs
  straight into the null dereference. That is a playback-creation failure under
  Wine, not a key-handling bug.

### 6.2 Test plan (rig copies only; zero risk to the real install)

The game copies in `$RIG/lan-{a,b,c}` are reflinked clones, so:

1. Reflink-copy `lan-c` aside once (e.g. `lan-c-movies-bak`) — do not use `rm`.
2. In `lan-c/movie/` only, rename `EV0001.wmv` and `EV0001JP.wmv`
   (`mv` to `*.wmv.disabled`).
3. Cold-boot slot C. Outcomes: (a) movie skipped → 109 s saved and today's crash
   class gone; (b) game shows an error or black screen → restore the names.
   The RTM table carries a hash per file, so (b) is a real possibility.
4. Orthogonal experiment: set `[MOVIE] nUseDXVA=0` in that prefix's `nioh.ini`
   and cold-boot. If the crash stops, keep it; if not, revert.
5. Do not touch `~/.local/share/Steam` or the real Documents.

Expected: **−109 s per boot** (plus the movie's own load) and removal of the one
crash we have seen today. Effort 1 h. Risk: low (armed copies only), medium
uncertainty (the game may insist on the file).

---

## 7. Option F — `rig/coop.py` (the driver)

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

* `do` — a short blind key sequence (taps are 80 ms apart: 4 taps ≈ 0.6 s).
* `expect` — a **predicate over the state oracle first**, vision second
  (`!= 0`, `phase==1`, `e8==2`, selected row index, screen template).
* `verify` after the step; on timeout, send `BackSpace` (verified: it backs out of
  menus, `docs/RIG.md:236`), re-read the screen, and retry the step at most twice.
* `timeout` budgets: title 30 s, load 120 s (the 3-instance iGPU stall can take
  300 s — see below), menu 5 s, join 180 s.
* On a third failure: screenshot + state dump + stop; never continue blind.

Deterministic sequences replace most vision:

* title: read selected row; press `Down` until CONTINUE is selected (max 3);
  verify; Return.
* map: `Down ×3` (SHRINE→TORII GATE) + Return; verify by the torii screen.
* torii: read the highlight, `Down ×1` (Yokai Realm → Random Encounters),
  re-read, Return; then the screen's own `Down` count + Return.
* shrine: hold `E` 1.7 s; verify `g+0xE5` (menu open) or the shrine-menu template.
* The map's fixed node list is why `map_select` and its 24-step OCR spiral can be
  deleted entirely.

### 7.3 Three-instance realities the driver must encode

* **Load is GPU-bound, not protocol-bound.** The second visitor's loader sits in
  phase 1 for 280–300 s because three games share the iGPU; `SIGSTOP`ing the
  other visitor in 5 s bursts cut a load to 33 s (`docs/FINDINGS.md:1170–1190`).
  The driver should implement this: whenever `phase==1` for the newcomer and the
  other visitor is in-mission, pause that other visitor's `nioh.exe` in 5 s bursts,
  `SIGCONT` at the end of every burst and before any other action. **Never pause
  the host.**
* **Re-join cost:** after a visitor leaves, the host's allowance stays 0 until
  co-op ends (`docs/FINDINGS.md:1200`). `$RIG/tools-tmp/budget-set.exe` already
  reopens it (documented in `docs/RIG.md:320`). Using it turns a ~60 s host menu
  replay into ~1 s — a diagnostic write, so gate it as a rig-only fast path.
* **Warm sessions beat relaunches.** Keep the host in the mission between tests
  and re-attach visitors; a cold launch costs the boot plus a 20 s mission load
  before any menu work, and a second visitor can add 33–300 s.
* **Order:** host first (pray → Summon Visitor → Make Offering), then visitors
  (Starting Point → Torii Gate → Random Encounters → Search).

### 7.4 Expected effect

| phase | now | after A+D+F |
|---|---|---|
| state check | 3.4–8 s | 0.00 s (memory) / 0.11–0.15 s (vision) |
| map navigation | up to 84 s, OCR-dependent | ~1 s, deterministic |
| menu step verify | ~5 s | ~0.12 s |
| join detection | OCR + 2.5 s sleeps | 10 ms slot-table poll |
| overall host+2 visitors (excluding game load) | minutes | ~5–10 s |
| re-join after a departure | ~60 s + menu replay | ~1–3 s (warm + budget-set) |

The remaining floor is game load: ~20 s for a first visitor, 33–300 s for a
second on this single iGPU.

---

## 8. Ranking and recommended plan

Ranked by (impact ÷ risk):

1. **F+A+D together** — memory oracle, fingerprints, deterministic `coop.py`.
   Removes all OCR and all fixed sleeps; every wait becomes an event.
2. **A alone** if only one thing is done now — the 10 ms poller already answers
   "loading / joined / allowance" and is proven.
3. **C** — save templates; cheap, removes the walk and map wandering.
4. **E** — movie experiment; cheap, 109 s/boot and the crash.
5. **Warm-session rejoin + `budget-set.exe`** — biggest per-test win, but a memory
   write (rig-only).
6. **B1 DirectInput injection** — only if input remains the flaky part.
7. **B2 script-binding calls** — not recommended.

### Recommended 3-step plan

**Step 1 (½–1 day).** Add `tools/rig/state.py` (dual backend: `/proc` when
`coop.py` launched the slot, else `admission-poll3.exe`), `tools/rig/vision.py`
(decode once; title rows, map rows, torii rows, search button; the measured boxes
in §5.2), and `tools/rig/coop.py` with the step contract. Replace
`nioh_nav.py`'s `classify/map_select/goto`. Delete the 2 s settle and the 2.5/3 s
sleeps. Acceptance: a full host+visitor setup with **zero OCR**, every transition
logged with its predicate and timing; a deliberate wrong-row start must be
detected and corrected (title must never press Return on NEW GAME).

**Step 2 (½ day).** Save templates per role + launch movie experiment. Create
`$RIG/saves/{b-host,a-visitor,c-visitor}/`, add `--restore-saves` to `coop.py`
(stopped game only), and run the EV0001 rename / `nUseDXVA=0` test on slot C.
Acceptance: host and visitors CONTINUE to the intended spot on 3/3 boots; slot C
boots without the DirectShow crash.

**Step 3 (optional, 1 day).** If input still misses or desyncs, add the
DirectInput wrapper (B1) as a research-build INI option; otherwise automate the
warm-session rejoin path (budget-set + SIGSTOP bursts) and measure a full 3-player
re-test cycle end to end.

---

## Appendix A — script-binding addresses (extracted from the merged dump)

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

Session/UI task layer (not script bindings; `docs/RE_SESSION_FLOW.md`):
host 0x140D71E00 · join-from-search 0x140D73240 · join-by-lobby-id
0x140D73380 (Steam) / 0x140D73480 (EOS) · search 0x140609D50.

## Appendix B — measured one-liners (reproduce)

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

## Appendix C — files/addresses cited

* `tools/rig/nioh_nav.py` 108–113 (2 s settle), 160–170 (RULES), 175–199
  (classify), 212 (2.5 s poll), 230 (blind Return), 243–256 (map_select crop).
* `docs/FINDINGS.md` 1068–1096 (admission table), 1096–1126 (3-player run),
  1170–1190 (load stall), 1165–1167 (`phase`, `Load::IsLoadEnd`), 1200 (allowance
  after departure); `docs/RIG.md` 194–200 (savedata tool), 236 (BackSpace),
  320 (tools-tmp pollers); `docs/RE_SESSION_FLOW.md` 113–179 (session struct),
  701–714 (UI task entry points), 717–750 (absences).
* `src/proxy/nioh3p_proxy.c` 2419 (session root), 2250–2270 (trace hooks),
  2555 (mode line).
* Disassembly `~/.cache/nioh3p/out/text.dis` (VMA 0x140000000), merged image
  `~/.cache/nioh3p/dumps/nioh3p_image_merged.bin`.
