#include "nioh3p_internal.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

static int g_host_nioh;     /* host basename is nioh.exe (not the harness host) */

/* ------------------------------------------------------------ main worker */


static DWORD WINAPI worker(LPVOID unused)
{
    HINSTANCE self = (HINSTANCE)unused;
    HMODULE pinned;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCSTR)(uintptr_t)worker, &pinned)) {
        OutputDebugStringA("nioh4p: cannot retain core module; mod inactive\n");
        return 0;
    }
    /* No file I/O under the loader lock: the config, the log and the banner are
     * opened here, never in DllMain. The critical section comes first, so LOG()
     * is safe from the first line on. */
    loglock_init();
    DWORD dlen = GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    if (!dlen || dlen >= MAX_PATH) {
        OutputDebugStringA("nioh4p: cannot resolve game directory; mod inactive\n");
        return 0;
    } else {
        char *slash = strrchr_a(g_dir, '\\');
        if (slash)
            *slash = 0;
    }
    cfg_load(g_dir);            /* records its outcome for cfg_report */
    log_init(g_dir);            /* the only log open in this process */
    LOG("=== nioh3p proxy loading: module=%p dir=%s ===", (void *)self, g_dir);
    if (g_log != INVALID_HANDLE_VALUE) {
        if (g_log_note[0])
            LOG("log: %s", g_log_note);
        else
            LOG("log: writing %s", g_log_path);
    }
    LOG(manifest_resource_present(self) ? "build: resource manifest present"
                                        : "build: no manifest resource");
#ifndef NIOH3P_RESEARCH
    /* Player build: the trace-only instrumentation and the dumper are not in
     * this binary. Report an ini that asks for them and continue in player
     * mode with all instrumented settings off. */
    if (g_cfg.trace) {
        LOG("config: trace=%d needs the research build; ignored", g_cfg.trace);
        g_cfg.trace = 0;
    }
    if (g_cfg.dump_image) {
        LOG("config: dump_image=%d needs the research build; ignored", g_cfg.dump_image);
        g_cfg.dump_image = 0;
    }
    if (g_cfg.redump_after_s) {
        LOG("config: redump_after_s=%d needs the research build; ignored",
            g_cfg.redump_after_s);
        g_cfg.redump_after_s = 0;
    }
    if (g_cfg.dump_every_s) {
        LOG("config: dump_every_s=%d needs the research build; ignored",
            g_cfg.dump_every_s);
        g_cfg.dump_every_s = 0;
    }
#endif
    cfg_report();

    g_base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h) {
        LOG("worker: cannot parse the main module PE headers at %p", g_base);
        return 0;
    }
    g_img_size = h->OptionalHeader.SizeOfImage;
    LOG("worker: main module base=%p size=%u entry=%#x sections=%d",
        g_base, (unsigned)g_img_size, (unsigned)h->OptionalHeader.AddressOfEntryPoint,
        (int)h->FileHeader.NumberOfSections);

    /* A different nioh.exe (after an update) must not receive the EOS/Steam
     * hooks: their detours read hard-coded struct offsets. The harness host
     * (testgame.exe) keeps the hooks: the game-side patches refuse anyway. */
    int mod_active = 1;
    if (g_host_nioh && !exe_supported()) {
        LOG("unsupported nioh.exe build (timestamp %#x); mod inactive - the game runs unmodified",
            h->FileHeader.TimeDateStamp);
        mod_active = 0;
    }
    unsigned addon_count = 0;

#ifdef NIOH3P_RESEARCH
    /* The on-disk copy exists only for the dump trigger: player mode never
     * reads the 27 MB nioh.exe. */
    size_t disk_size = 0;
    uint8_t *disk = pe_disk_probe(&disk_size);
#endif

    /* hooks first: they are cheap and must be in place before the game opens
     * its first lobby, while the image dump can take a moment */
    if (g_cfg.hook_eos && mod_active) {
        install_game_capacity_hook();
        /* EOSSDK may not be mapped yet: the loader resolves imports in order and
         * dinput8.dll can precede EOSSDK-Win64-Shipping.dll. Poll quickly so the
         * hooks are in place before the game can open a lobby. Once the module
         * is mapped, every configured name is either resolved or absent, so a
         * later poll cannot change anything. */
        for (int tries = 0; tries < 2000; tries++) {
            if (eos_unresolved())
                install_eos_hooks();
            if (!g_steam_ready && tries < 600)      /* Steam is optional: 3 s */
                install_steam_hooks();
            int eos_waiting = eos_unresolved() && !eos_module_seen();
            if (!eos_waiting && (g_steam_ready || tries >= 600))
                break;
            Sleep(5);
        }
        if (eos_unresolved()) {
            char names[400];
            eos_unresolved_names(names, (int)sizeof names);
            LOG("eos: giving up on %d hook(s) (module %s): %s",
                eos_unresolved(), eos_module_seen() ? "mapped" : "not mapped", names);
        }
        /* Informational, not a degradation: the build simply never calls these
         * exports, so there is no call site to hook. */
        if (eos_nocall_count()) {
            char names[400];
            eos_nocall_names(names, (int)sizeof names);
            LOG("eos: %d hook(s) have no call site in this build: %s",
                eos_nocall_count(), names);
        }
        LOG("worker: steam ready=%d create slot(one-based)=%u setmax slot=%u",
            g_steam_ready, g_steam[0].slot_index, g_steam[1].slot_index);
        LOG("worker: %d EOS hook(s) installed", g_eos_hooks);
        LOG("mode: %s (trace=%d): %d steam method(s) configured (patched once the "
            "game requests the interface), %d EOS hook(s); "
            "P2P/friends/callback tracing %s",
            g_cfg.trace ? "research" : "player", g_cfg.trace,
            steam_method_count(), g_eos_hooks, g_cfg.trace ? "on" : "off");
    }
    /* Catch native Steam interface creation before optional DLL/config I/O. */
    if (g_cfg.hook_eos && mod_active) {
        const Nioh4pCoreApi api = {
            .version = NIOH4P_ADDON_ABI,
            .size = sizeof(Nioh4pCoreApi),
            .game_base = (uintptr_t)g_base,
            .max_players = g_cfg.max_players,
            .trace = (uint32_t)g_cfg.trace,
            .log = LOG,
            .readable = readable,
            .publish_code16 = publish_code16,
            .alloc_rel32 = alloc_rel32,
        };
        addon_count = nioh4p_load_addons(&api, g_dir, &g_local_rules, inline_install);
        rules_apply();
    }

    /* One line a player can be asked to read back: it names every part of the
     * mod that is in place, and a second line names what is not. */
    {
        int exe_ok = exe_supported();
        int active = g_cfg.hook_eos && mod_active;
        int patched = g_game_create_param_hook.nslots > 0;
        int configured = active ? eos_expected() : 0;
        LOG("preflight: exe=%s eos=%d/%d steam=%d game_patch=%s log=%s",
            exe_ok ? "ok" : "unsupported", g_eos_hooks, configured,
            steam_method_count(), patched ? "ok" : "skipped",
            g_log != INVALID_HANDLE_VALUE ? g_log_path : "none");
        char why[320];
        int wn = 0;
        if (!exe_ok)
            wn += fmt(why + wn, (int)sizeof why - wn, "exe=unsupported");
        if (!g_cfg.hook_eos)
            wn += fmt(why + wn, (int)sizeof why - wn, "%shook_eos=0", wn ? ", " : "");
        if (g_eos_hooks < configured)
            wn += fmt(why + wn, (int)sizeof why - wn, "%seos hooks %d/%d",
                      wn ? ", " : "", g_eos_hooks, configured);
        if (active && !patched)
            wn += fmt(why + wn, (int)sizeof why - wn, "%sgame patch not installed",
                      wn ? ", " : "");
        if (g_log == INVALID_HANDLE_VALUE)
            wn += fmt(why + wn, (int)sizeof why - wn, "%slog not created", wn ? ", " : "");
        if (!g_steam_ready)
            wn += fmt(why + wn, (int)sizeof why - wn, "%ssteam interface not seen",
                      wn ? ", " : "");
        if (wn)
            LOG("preflight: INCOMPLETE %s", why);
    }

#ifdef NIOH3P_RESEARCH
    /* The dump schedule can take 36 minutes; on this thread it held back the
     * allowance installer, so a research host refused its second visitor.
     * The dump thread owns the on-disk copy from here on. */
    if (g_cfg.dump_image) {
        pe_dump_handoff(disk, disk_size);
        disk = 0;
        HANDLE t = CreateThread(NULL, 0, dump_worker, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            LOG("worker: cannot start the dump thread (err %lu)", GetLastError());
    }
    if (disk)
        VirtualFree(disk, 0, MEM_RELEASE);
#endif
    /* Companion hooks: the 3-4 player fixes, and in research builds with
     * trace=1 the game packet trace that rides on the same entry points. */
    int want_tokoyo = g_cfg.max_players >= 3;
#ifdef NIOH3P_RESEARCH
    want_tokoyo = want_tokoyo || g_cfg.trace;
#endif
    if (want_tokoyo && g_cfg.hook_eos && mod_active && g_game_create_param_hook.nslots) {
        HANDLE t = CreateThread(NULL, 0, tokoyo_hooks_worker, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            LOG("game tokoyo: cannot start the hook thread (err %lu)", GetLastError());
    }
    if (g_cfg.hook_eos && mod_active) {
        HANDLE t = CreateThread(NULL, 0, rules_worker, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            LOG("rules: cannot start the lobby-rule thread (err %lu)", GetLastError());
        if (addon_count) {
            t = CreateThread(NULL, 0, nioh4p_addons_worker, NULL, 0, NULL);
            if (t)
                CloseHandle(t);
            else
                LOG("addons: cannot start the native-hook thread (err %lu)", GetLastError());
        }
        install_game_visitor_allowance();
        install_game_invite_hooks();
    }

    if (g_cfg.hook_eos && mod_active) {
        for (int i = 0; i < 24; i++) {          /* 2 minutes of restoration watch */
            Sleep(5000);
            int fixed = steam_repair();
            fixed += repair_slots(&g_game_create_param_hook);
            for (int k = 0; k < g_hook_count; k++)
                fixed += repair_slots(&g_hooks[k]);
            if (fixed)
                LOG("worker: re-applied %d restored hook slot(s)", fixed);
        }
    }
    LOG("worker: done");
    return 0;
}

/* ----------------------------------------------------- dinput8 forwarding */

static HMODULE g_real_dinput8;

static HMODULE real_dinput8(void)
{
    if (g_real_dinput8)
        return g_real_dinput8;
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n >= MAX_PATH - 20)
        return NULL;
    strcat_a(path, "\\dinput8.dll");
    /* Only the system copy: LoadLibraryA("dinput8.dll") could resolve to this
     * proxy again (it is named dinput8.dll) and recurse into itself. */
    g_real_dinput8 = LoadLibraryA(path);
    return g_real_dinput8;
}

typedef HRESULT (WINAPI *PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI *PFN_DllUnregisterServer)(void);
typedef HRESULT (WINAPI *PFN_DllInstall)(BOOL, PCWSTR);

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    HMODULE m = real_dinput8();
    PFN_DirectInput8Create fn = m ? (PFN_DirectInput8Create)(uintptr_t)GetProcAddress(m, "DirectInput8Create") : 0;
    if (!fn) {
        LOG("DirectInput8Create: real dinput8 unavailable (module %p)", (void *)m);
        return E_FAIL;
    }
    LOG("DirectInput8Create(version=%#x): forwarded to %p", (unsigned)version, (void *)fn);
    return fn(hinst, version, riid, out, outer);
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    HMODULE m = real_dinput8();
    PFN_DllGetClassObject fn = m ? (PFN_DllGetClassObject)(uintptr_t)GetProcAddress(m, "DllGetClassObject") : 0;
    return fn ? fn(rclsid, riid, ppv) : E_FAIL;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    /* The proxy is statically imported and keeps the game's IAT and vtables
     * patched: it must never be unloaded. */
    return S_FALSE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    HMODULE m = real_dinput8();
    PFN_DllRegisterServer fn = m ? (PFN_DllRegisterServer)(uintptr_t)GetProcAddress(m, "DllRegisterServer") : 0;
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    HMODULE m = real_dinput8();
    PFN_DllUnregisterServer fn = m ? (PFN_DllUnregisterServer)(uintptr_t)GetProcAddress(m, "DllUnregisterServer") : 0;
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI DllInstall(BOOL install, PCWSTR cmdline)
{
    HMODULE m = real_dinput8();
    PFN_DllInstall fn = m ? (PFN_DllInstall)(uintptr_t)GetProcAddress(m, "DllInstall") : 0;
    return fn ? fn(install, cmdline) : E_FAIL;
}

/* Not exported by Wine's dinput8; kept for parity with the Windows DLL. */
LPCDIDATAFORMAT WINAPI GetdfDIJoystick(void)
{
    HMODULE m = real_dinput8();
    if (!m)
        return NULL;
    typedef LPCDIDATAFORMAT (WINAPI *PFN)(void);
    PFN fn = (PFN)(uintptr_t)GetProcAddress(m, "GetdfDIJoystick");
    return fn ? fn() : NULL;
}

/* ------------------------------------------------------------------ DllMain */
static HANDLE g_core_instance;


BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        /* nioh_launcher.exe also imports dinput8.dll: only nioh.exe and the harness host (tools/testgame/) get the mod. */
        char host[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, host, MAX_PATH);
        if (!n || n >= MAX_PATH)
            return TRUE;        /* cannot identify the host: do nothing */
        char *base = strrchr_a(host, '\\');
        base = base ? base + 1 : host;
        if (stricmp_a(base, "nioh.exe") && stricmp_a(base, "testgame.exe"))
            return TRUE;
        g_host_nioh = !stricmp_a(base, "nioh.exe");
        char name[80];
        fmt(name, sizeof name, "Local\\nioh4p-core-%lu", GetCurrentProcessId());
        SetLastError(0);
        HANDLE instance = CreateMutexA(NULL, FALSE, name);
        if (!instance) {
            OutputDebugStringA("nioh4p: cannot establish single core instance\n");
            return TRUE;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(instance);
            OutputDebugStringA("nioh4p: another core instance is already active\n");
            return TRUE;
        }
        g_core_instance = instance;
        /* Nothing here opens a file: the loader lock is held. worker() loads the
         * config, opens the log, prints the banner and reports every failure. */
        loglock_init();
        HANDLE t = CreateThread(NULL, 0, worker, (LPVOID)self, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            /* No worker, so no log: the debugger is the only sink left. */
            OutputDebugStringA("nioh3p: CreateThread failed; mod inactive\n");
    }
    return TRUE;
}
