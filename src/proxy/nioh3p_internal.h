/*
 * nioh3p - Nioh: Complete Edition online research / co-op player-cap mod
 *
 * Loaded as a dinput8.dll proxy: nioh.exe statically imports dinput8.dll, so the
 * Windows loader maps this DLL before the game's entry point - i.e. before the
 * VMProtect stub unpacks the encrypted .text section.
 *
 * What it does (nothing is faked):
 *   1. Forwards every real dinput8 export to the system dinput8.
 *   2. Research build only, with dump_image=1: waits for VMProtect to decrypt
 *      .text, then dumps the *runtime* module image. nioh.exe ships with an
 *      encrypted .text (entropy 8.0), so the game's code can neither be
 *      analysed nor patched statically - this dump is the prerequisite for any
 *      code-level mod.
 *   3. Hooks the EOS lobby API and raises MaxLobbyMembers / MaxMembers to the
 *      configured player count. EOSSDK-Win64-Shipping.dll and the game's import
 *      table are not encrypted, so this works without knowing the netcode.
 *      Two mechanisms: the documented IAT slot, plus a fallback that scans the
 *      main module for stored copies of the EOS entry point (a rebuilt or
 *      protected import table would defeat the first one).
 *   4. Patches the game's own two-player assumptions once .text is plaintext:
 *      the visitor allowance (shrine and Torii Gate recruitment) and, for the
 *      Torii Gate companion mode, two inline hooks (the host mirrors the
 *      preparation packets to every companion; later companions leave the
 *      shared mission-start counter to slot 1). See docs/RE_TOKOYO_COOP.md.
 *   5. Host-authoritative lobby rules (enemy_hp_percent,
 *      assist_drain_percent, respawn_at_shrine): the host publishes one lobby
 *      data value and a client of that lobby uses it instead of its own file;
 *      the local file applies only to a lobby this process confirmed it owns,
 *      and solo play keeps the stock numbers. Three small native detours apply
 *      the values (character status max HP, the inline assist drain, and the
 *      pending respawn point). See docs/FINDINGS.md, "Lobby gameplay rules".
 *
 * Built freestanding: no CRT and no api-ms-win-crt-* imports, because Proton's
 * Wine (like plain Wine) does not ship those apisets - a CRT-linked DLL would
 * stop the game from starting. Only KERNEL32 is imported.
 *
 * Config: nioh3p.ini next to this DLL. Log: nioh3p.log in out_dir when that
 * key is set, otherwise next to this DLL. When the log cannot be written
 * there, the proxy retries next to this DLL and finally in %TEMP% (the
 * directory GetTempPathA reports, never the Windows directory), so a read-only
 * install still leaves a log.
 *
 * Two build variants. The default build is the player build that
 * tools/package.sh and install.sh ship: the trace-only instrumentation and the
 * image dumper are compiled out entirely. -DNIOH3P_RESEARCH=1 produces the
 * research build (build/research/dinput8.dll, built by the same ./build.sh).
 *
 * Within the research build, two runtime modes are selected by the `trace`
 * key in nioh3p.ini:
 *   trace=0 (player mode, the default): only the hooks that change behaviour
 *     (lobby capacity, filter drops, force_unlock, the metadata guard, the
 *     companion hooks) plus the rare lobby events (create/join/leave) and the
 *     numerical search filter (forwarded unchanged, but its key names go to
 *     the log) are installed. The
 *     per-P2P-packet, friends, socket and callback instrumentation is not
 *     installed at all - no hook, no log line - so the network path is
 *     untouched and a multi-hour session writes a small log.
 *   trace=1 (research mode): the full instrumentation of the original build:
 *     every hooked Steam/EOS call, its call-site note and a stack backtrace.
 *     The log grows by ~2,000 lines per second during a mission.
 * The log is opened once per process: an existing nioh3p.log is moved to
 * nioh3p.prev.log first, so the current and the previous launch can both be
 * sent. Image dumps always stay in out_dir (or the game directory); they never
 * follow the log to %TEMP%.
 *
 * Source layout: the former single-translation-unit nioh3p_proxy.c is split
 * along its section seams into the modules next to this header (util, log,
 * config, pe, patch, eos, steam, rules, gamepatch, gamehooks, hud, prep,
 * main). This header is the only thing they share; every symbol that crosses
 * a module boundary is declared here and loses its `static`.
 */

#ifndef NIOH3P_INTERNAL_H
#define NIOH3P_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include "../common/freestanding.h"
#include "../common/nioh4p_addon.h"
#include "addons.h"

#ifndef EOS_CALL
#define EOS_CALL __stdcall
#endif

/* -DNIOH3P_RESEARCH=1 builds the research instrumentation (trace-only hooks,
 * backtraces, call-site names, image dumper). The player build leaves all of
 * it out of the binary; build.sh writes it to build/research/dinput8.dll. */

/* ------------------------------------------------------------------ config */

typedef struct {
    uint32_t max_players;
    int dump_image;
    int redump_after_s;
    int dump_every_s;
    int hook_eos;
    /* 0 = keep the game's own search filters (stock behaviour): the stock
     * "locked" filter still hides hosting sessions. 1 is an opt-in workaround
     * for a host that cannot be found and swallows the ktol_*lock_status
     * filter. drop_slots_filter is the same opt-in workaround for the stock
     * "free slots" filter that hides full sessions. */
    int drop_lock_filter;
    int force_unlock;       /* host: advertise the session as unlocked */
    int drop_slots_filter;
    int trace;              /* 0 = player mode, 1 = full research instrumentation */
    /* 0 = off (default): the third companion row does not exist at all, so a
     * 4-player session behaves exactly like the shipped two-row build. 1 opts
     * into the third row (element 0x73, private vtable, block C). */
    int hud_row3;
} Config;

extern Config g_cfg;    /* written only by cfg_load, before any hook exists */

/* Slot table and hook-table capacities shared by the patch machinery. The
 * research build with trace=1 installs 12 hooks (EOS + Steam + kernel32
 * exit tracing); 16 leaves headroom for one more interface without the
 * table-full failure silently dropping the last-installed hook. */
#define MAX_SLOTS  96
#define MAX_HOOKS  16

typedef struct {
    const char *name;
    void *target;               /* resolved EOS entry point we look for */
    void *detour;
    void *orig;
    int nslots;
    void **slot[MAX_SLOTS];
} Hook;

typedef Nioh4pHook InlineHook;

/* Why install_hook last returned 0. The EOS installer needs to tell "this
 * build never calls the export" (known, harmless) from a real failure. */
enum { HOOK_FAIL_NONE = 0, HOOK_FAIL_TABLE_FULL, HOOK_FAIL_NO_CALLSITE };
extern int g_hook_fail;

extern Hook g_game_create_param_hook;


/* Steam vtable hook table (steam.c); main.c and rules.c read it by index. */
typedef struct {
    const char *name;
    void *det;
    unsigned slot_index;
    int trace_only;             /* installed only when trace=1 */
    void *orig;
    void **slot;
} SteamHook;
extern SteamHook g_steam[];

/* IAT-slot hook table (patch.c); main.c re-applies restored slots. */
extern Hook g_hooks[MAX_HOOKS];
extern int g_hook_count;

/* log.c: the out_dir override buffer config.c fills from the ini. */
extern char g_out[MAX_PATH];
/* -------------------------------------------------------- shared game RVAs */

#define GAME_ROOT_RVA      0x1790a70u  /* session root; +0x3c8 = own slot index */
#define GAME_COOP_MODE_RVA 0x191de34u  /* 1 while a Tokoyo companion session prepares */
#define GAME_MANAGER_RVA   0x18715e0u  /* online manager; +0x5b0 role, +0x660 Tokoyo */
#define GAME_LOADER_OFF        0x5d0u   /* manager +0x5d0: mission loader phase */
#define GAME_LOADER_IN_MISSION 0xcu     /* 0xC = in the mission (docs/RE_TOKOYO_COOP.md) */

#define HUD_ROW_CTOR_RVA   0xc1e230u
#define PREP_UPDATE_RVA    0xd2d900u

/* ------------------------------------------------------------- util.c */

int stricmp_a(const char *a, const char *b);
char *strrchr_a(const char *s, int c);
char *strstr_a(const char *h, const char *n);
char *strcat_a(char *d, const char *s);
int vfmt(char *dst, int cap, const char *f, va_list ap);
int fmt(char *dst, int cap, const char *f, ...);

/* -------------------------------------------------------------- log.c */

void loglock_init(void);
void LOG(const char *f, ...);
void log_init(const char *game_dir);

extern char g_dir[MAX_PATH];            /* game directory (worker fills it) */
extern char g_log_path[MAX_PATH];       /* log file actually in use */
extern char g_log_note[MAX_PATH];       /* fallback explanation, empty when none */
extern HANDLE g_log;                    /* INVALID_HANDLE_VALUE until opened */
extern const char *g_out_dir;           /* where the log and the dumps go */

#ifdef NIOH3P_RESEARCH
void bt_resolve(void);
void log_backtrace(const char *what);
const char *site_at(void *ret, char *buf, int cap);
const char *site_exact(void *addr, char *buf, int cap);
#endif

/* ------------------------------------------------------------ config.c */

int parse_int_strict(const char *s, int *out);
void cfg_load(const char *dir);
void cfg_report(void);

/* ---------------------------------------------------------------- pe.c */

extern uint8_t *g_base;
extern size_t g_img_size;

IMAGE_NT_HEADERS64 *nt(void);
int exe_supported(void);
int manifest_resource_present(HMODULE self);

#ifdef NIOH3P_RESEARCH
DWORD WINAPI dump_worker(LPVOID unused);
/* Read the on-disk exe for the dump trigger and log its .text entropy. */
uint8_t *pe_disk_probe(size_t *size_out);
/* Hand the buffer to the dump worker; it owns it from here on. */
void pe_dump_handoff(uint8_t *disk, size_t size);
#endif

/* -------------------------------------------------------------- patch.c */

int readable(const void *p, size_t n);
int executable_ptr(const void *p);
int add_slot(Hook *h, void **slot);
int repair_slots(Hook *h);
int install_hook(const char *dll_name, const char *name, void *target,
                 void *detour, void **orig_out);
int publish_code16(uint8_t *site, const uint8_t *expected,
                   const uint8_t *patch, const char *name);
uint8_t *alloc_rel32(const uint8_t *site, SIZE_T size);
void abs_jmp(uint8_t *at, const void *to);
int inline_install(const InlineHook *h);

/* ---------------------------------------------------------------- eos.c */

extern int g_eos_hooks;     /* successfully installed EOS entry-point hooks */
extern int g_eos_polls;     /* install_eos_hooks calls that saw the module */

int eos_expected(void);
int eos_nocall_count(void);
void eos_nocall_names(char *dst, int cap);
int eos_unresolved(void);
void eos_unresolved_names(char *dst, int cap);
void install_eos_hooks(void);

/* -------------------------------------------------------------- steam.c */

extern int g_steam_ready;

int steam_method_count(void);
int steam_repair(void);
void install_steam_hooks(void);
/* One SteamMatchMaking009 method by zero-based vtable index and g_steam
 * table index; see rules.c for the only cross-module user. */
void *steam_mm_method(void *self, unsigned vtable_index, int table_index);

#ifdef NIOH3P_RESEARCH
void p2p_head(char *out, size_t cap, const void *data, unsigned size);
int pkt_should_log(int dir, unsigned id);
#endif

/* -------------------------------------------------------------- rules.c */

typedef Nioh4pRules Rules;
extern Rules g_local_rules;

void rules_note_create_request(void);
void rules_publish(void *self, uint64_t lobby);
void rules_join_lobby(void *self, uint64_t lobby);
void rules_left_lobby(uint64_t lobby);
void rules_apply(void);
DWORD WINAPI rules_worker(LPVOID unused);

/* ---------------------------------------------------------- gamepatch.c */

void install_game_capacity_hook(void);
void install_game_metadata_guard(void);
void install_game_visitor_allowance(void);
uint8_t *game_manager_ptr(void);
void install_game_invite_hooks(void);

#ifdef NIOH3P_RESEARCH
void det_SteamRunCallbacks(void);
extern void (*orig_RunCallbacks)(void);
#endif

/* --------------------------------------------------------- gamehooks.c */

DWORD WINAPI tokoyo_hooks_worker(LPVOID unused);

/* ---------------------------------------------------------------- hud.c */

extern void *(*g_hud_row_ctor)(void *self, int id, int index);

unsigned relocate_hud_row(uint8_t *out);
void *det_HudRow(void *self, int id, int index);

/* ---------------------------------------------------------------- prep.c */

extern uintptr_t (*g_prep_update)(void *self, void *context);

uintptr_t det_PrepUpdate(void *self, void *context);
unsigned relocate_prep_update(uint8_t *out);
int install_prep_roster(void);

/* ------------------------------------------------------------- research */

#ifdef NIOH3P_RESEARCH
#define SITE_NOTE(label) do { \
    if (g_cfg.trace) { \
        char sb_[240]; \
        LOG("site: %s from %s", (label), site_at(__builtin_return_address(0), sb_, sizeof sb_)); \
    } \
} while (0)
#else
#define SITE_NOTE(label) ((void)0)
#endif

#endif
