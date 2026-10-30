#ifndef NIOH4P_ADDON_H
#define NIOH4P_ADDON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define NIOH4P_ADDON_ABI 1u
#define NIOH4P_MAX_ADDON_HOOKS 16u

enum Nioh4pAddonId {
    NIOH4P_SHRINES = 0,
    NIOH4P_KODAMA,
    NIOH4P_ENEMY_HEALTH,
    NIOH4P_ASSIST_GAUGE,
    NIOH4P_SHRINE_RESPAWN,
    NIOH4P_ADDON_COUNT
};

typedef struct {
    int enemy_hp_pct;
    int assist_drain_pct;
    int respawn_shrine;
} Nioh4pRules;

typedef struct {
    uint32_t rva;
    uint8_t n;
    const char *name;
    void *detour;
    void **orig;
    uint8_t bytes[24];
    unsigned (*relocate)(uint8_t *out);
} Nioh4pHook;

typedef struct {
    uint32_t version;
    uint32_t size;
    uintptr_t game_base;
    uint32_t max_players;
    uint32_t trace;
    void (*log)(const char *format, ...);
    int (*readable)(const void *address, size_t size);
    int (*publish_code16)(uint8_t *site, const uint8_t *expected,
                          const uint8_t *patch, const char *name);
    uint8_t *(*alloc_rel32)(const uint8_t *site, SIZE_T size);
} Nioh4pCoreApi;

typedef struct {
    uint32_t version;
    uint32_t size;
    uint32_t id;
    uint32_t enabled;
    const Nioh4pHook *hooks;
    uint32_t hook_count;
    int local_value;
    /* 1 installed, 0 waiting for plaintext, -1 installation failed. */
    int (*install_patch)(void);
    /* Borrowed value; copy required fields before returning. Called outside
     * the session lock. Native hooks must use their own atomic field cache. */
    void (*rules_changed)(const Nioh4pRules *rules);
} Nioh4pAddon;

/* Initialization runs on the core worker, never under the loader lock.
 * Return a static descriptor. Do not install hooks or start worker threads
 * here: the core validates the complete descriptor before installing it.
 * The core retains accepted modules for the lifetime of the game process. */
typedef const Nioh4pAddon *(__cdecl *Nioh4pAddonInitFn)(
    const Nioh4pCoreApi *core, HMODULE module);

#endif
