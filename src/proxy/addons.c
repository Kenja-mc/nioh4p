#include "addons.h"
#include "../common/freestanding.h"

typedef struct {
    HMODULE module;
    const Nioh4pAddon *descriptor;
    const char *name;
    unsigned first_hook;
} LoadedAddon;

static const char *const addon_names[NIOH4P_ADDON_COUNT] = {
    "nioh4p-shrines", "nioh4p-kodama", "nioh4p-enemy-health",
    "nioh4p-assist-gauge", "nioh4p-shrine-respawn"
};
static Nioh4pCoreApi g_api;
static LoadedAddon g_loaded[NIOH4P_ADDON_COUNT];
static unsigned g_count, g_hook_count;
static int (*g_install_hook)(const Nioh4pHook *hook);

static int descriptor_valid(const Nioh4pAddon *addon, unsigned id)
{
    if (!addon || !g_api.readable(addon, sizeof *addon) ||
        addon->version != NIOH4P_ADDON_ABI || addon->size != sizeof *addon ||
        addon->id != id || addon->enabled > 1)
        return 0;
    if (!addon->enabled)
        return 1;
    if ((!addon->hook_count && !addon->install_patch) ||
        addon->hook_count > NIOH4P_MAX_ADDON_HOOKS - g_hook_count ||
        (addon->hook_count && !g_api.readable(addon->hooks,
                            addon->hook_count * sizeof *addon->hooks)))
        return 0;
    if ((id == NIOH4P_ENEMY_HEALTH &&
         (addon->local_value < 25 || addon->local_value > 400)) ||
        (id == NIOH4P_ASSIST_GAUGE &&
         (addon->local_value < 0 || addon->local_value > 1000)) ||
        (id == NIOH4P_SHRINE_RESPAWN &&
         (addon->local_value < 0 || addon->local_value > 1)))
        return 0;
    for (unsigned i = 0; i < addon->hook_count; i++) {
        const Nioh4pHook *hook = &addon->hooks[i];
        if (hook->n < 14 || hook->n > sizeof hook->bytes ||
            (hook->rva & 15) || !hook->detour || !hook->orig || !hook->name)
            return 0;
        for (unsigned j = 0; j < i; j++)
            if (addon->hooks[j].rva == hook->rva)
                return 0;
        for (unsigned j = 0; j < g_count; j++)
            for (unsigned k = 0; k < g_loaded[j].descriptor->hook_count; k++)
                if (g_loaded[j].descriptor->hooks[k].rva == hook->rva)
                    return 0;
    }
    return 1;
}

unsigned nioh4p_load_addons(const Nioh4pCoreApi *api, const char *game_directory,
                           Nioh4pRules *local_rules,
                           int (*install_hook)(const Nioh4pHook *hook))
{
    g_api = *api;
    g_install_hook = install_hook;
    const size_t directory_length = strlen(game_directory);
    for (unsigned id = 0; id < NIOH4P_ADDON_COUNT; id++) {
        char path[MAX_PATH];
        const char directory[] = "\\DLLMods\\";
        const size_t name_length = strlen(addon_names[id]);
        const size_t prefix = directory_length + sizeof directory - 1;
        if (prefix + name_length + sizeof ".dll" > sizeof path) {
            g_api.log("addons: path too long for %s; not loaded", addon_names[id]);
            continue;
        }
        memcpy(path, game_directory, directory_length);
        memcpy(path + directory_length, directory, sizeof directory - 1);
        memcpy(path + prefix, addon_names[id], name_length);
        memcpy(path + prefix + name_length, ".dll", sizeof ".dll");
        DWORD attributes = GetFileAttributesA(path);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                g_api.log("addons: cannot inspect %s (err %lu)", addon_names[id], error);
            continue;
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            g_api.log("addons: %s is not a DLL file; not loaded", addon_names[id]);
            continue;
        }
        HMODULE module = LoadLibraryExA(path, NULL,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            g_api.log("addons: cannot load %s (err %lu)", addon_names[id], GetLastError());
            continue;
        }
        Nioh4pAddonInitFn initialize = (Nioh4pAddonInitFn)(uintptr_t)
            GetProcAddress(module, "Nioh4pAddonInit");
        const Nioh4pAddon *addon = initialize ? initialize(&g_api, module) : NULL;
        if (!descriptor_valid(addon, id)) {
            g_api.log("addons: %s initialization or ABI rejected; no hooks registered",
                      addon_names[id]);
            FreeLibrary(module);
            continue;
        }
        if (!addon->enabled) {
            g_api.log("addons: %s disabled by its own config; no hooks registered",
                      addon_names[id]);
            FreeLibrary(module);
            continue;
        }
        LoadedAddon *loaded = &g_loaded[g_count++];
        loaded->module = module;
        loaded->descriptor = addon;
        loaded->name = addon_names[id];
        loaded->first_hook = g_hook_count;
        g_hook_count += addon->hook_count;
        if (id == NIOH4P_ENEMY_HEALTH) local_rules->enemy_hp_pct = addon->local_value;
        else if (id == NIOH4P_ASSIST_GAUGE) local_rules->assist_drain_pct = addon->local_value;
        else if (id == NIOH4P_SHRINE_RESPAWN) local_rules->respawn_shrine = addon->local_value;
        g_api.log("addons: %s accepted (%u hook(s), custom patch %s)",
                  loaded->name, addon->hook_count, addon->install_patch ? "yes" : "no");
    }
    if (!g_count)
        g_api.log("addons: core only; no optional gameplay hooks registered");
    return g_count;
}

void nioh4p_addons_rules_changed(const Nioh4pRules *rules)
{
    static Nioh4pRules previous;
    static int seen;
    if (seen && previous.enemy_hp_pct == rules->enemy_hp_pct &&
        previous.assist_drain_pct == rules->assist_drain_pct &&
        previous.respawn_shrine == rules->respawn_shrine)
        return;
    previous = *rules;
    seen = 1;
    for (unsigned i = 0; i < g_count; i++)
        if (g_loaded[i].descriptor->rules_changed)
            g_loaded[i].descriptor->rules_changed(rules);
}

DWORD WINAPI nioh4p_addons_worker(LPVOID unused)
{
    (void)unused;
    int state[NIOH4P_MAX_ADDON_HOOKS] = {0};
    int patch_state[NIOH4P_ADDON_COUNT] = {0};
    for (unsigned waited = 0; waited < 600000; waited += 50) {
        unsigned pending = 0;
        for (unsigned i = 0; i < g_count; i++) {
            const LoadedAddon *loaded = &g_loaded[i];
            const Nioh4pAddon *addon = loaded->descriptor;
            for (unsigned j = 0; j < addon->hook_count; j++) {
                int *installed = &state[loaded->first_hook + j];
                if (*installed)
                    continue;
                *installed = g_install_hook(&addon->hooks[j]);
                if (*installed < 0)
                    g_api.log("addons: %s/%s installation failed", loaded->name,
                              addon->hooks[j].name);
                else if (*installed)
                    g_api.log("addons: %s/%s installed", loaded->name,
                              addon->hooks[j].name);
                else
                    pending++;
            }
            if (addon->install_patch && !patch_state[i]) {
                patch_state[i] = addon->install_patch();
                if (patch_state[i] < 0)
                    g_api.log("addons: %s custom patch installation failed", loaded->name);
                else if (!patch_state[i])
                    pending++;
            }
        }
        if (!pending)
            break;
        Sleep(50);
    }
    for (unsigned i = 0; i < g_count; i++) {
        const LoadedAddon *loaded = &g_loaded[i];
        const Nioh4pAddon *addon = loaded->descriptor;
        unsigned installed = 0;
        for (unsigned j = 0; j < addon->hook_count; j++)
            installed += state[loaded->first_hook + j] == 1;
        int patch_ok = !addon->install_patch || patch_state[i] == 1;
        g_api.log("addons: %s %s: %u/%u native hooks, custom patch %s",
                  loaded->name, installed == addon->hook_count && patch_ok ?
                  "ready" : "INCOMPLETE", installed, addon->hook_count,
                  !addon->install_patch ? "not needed" : patch_ok ? "ready" : "failed");
    }
    return 0;
}
