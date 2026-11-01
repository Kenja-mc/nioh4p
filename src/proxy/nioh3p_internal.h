/*
 * nioh4p - Nioh: Complete Edition co-op player-cap mod, loaded as a dinput8
 * proxy: nioh.exe statically imports dinput8.dll, so this DLL maps before the
 * game's entry point, before the VMProtect stub decrypts .text.
 *
 * It forwards every real dinput8 export, hooks the EOS lobby API to raise
 * MaxLobbyMembers/MaxMembers (IAT slot plus a main-module scan fallback),
 * patches the game's two-player assumptions once .text is plaintext (visitor
 * allowance, Torii Gate companion hooks, host-authoritative lobby rules), and
 * in the research build dumps the runtime image and traces Steam/EOS calls.
 * Nothing is faked; docs/FINDINGS.md and docs/RE_TOKOYO_COOP.md carry the
 * evidence.
 *
 * Freestanding: no CRT and no api-ms-win-crt-* imports - Proton's Wine ships
 * none, so a CRT-linked DLL stops the game from starting. Kernel32 only.
 *
 * Config: nioh3p.ini next to this DLL. The log goes to out_dir when set, else
 * next to the DLL, else %TEMP%; an existing nioh3p.log moves to
 * nioh3p.prev.log at startup. Image dumps never follow the log to %TEMP%.
 * The player build compiles the trace instrumentation and dumper out entirely;
 * -DNIOH3P_RESEARCH=1 produces the research build whose `trace` key selects
 * player mode (0, default) or full instrumentation (1) at runtime.
 *
 * This header is the only shared declaration point: every symbol crossing a
 * module boundary is declared here.
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

/* ------------------------------------------------------------------ config */

typedef struct {
    uint32_t max_players;
    int dump_image;
    int redump_after_s;
    int dump_every_s;
    int hook_eos;
    /* 1 = opt-in workaround that swallows the stock "locked" and "free slots"
     * search filters, which hide hosting and full sessions. 0 keeps stock. */
    int drop_lock_filter;
    int force_unlock;       /* host: advertise the session as unlocked */
    int drop_slots_filter;
    int trace;              /* 0 = player mode, 1 = full research instrumentation */
    /* 1 = the third companion row (element 0x73, private vtable, block C). */
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

int eos_module_seen(void);
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
