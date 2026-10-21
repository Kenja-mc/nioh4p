/*
 * Fake steam_api64.dll for the nioh3p harness - shaped like the shipped one.
 *
 * Flat exports dispatch through interface vtables owned by steamclient64.dll.
 * CSteamID-returning exports also marshal a hidden return buffer; they are not
 * interchangeable with the virtual methods that the proxy patches.
 *
 * These fixtures exercise selected ABI shapes, not the live Steam service:
 *   fake_steam_thunks.S     dispatch instructions from the shipped DLL
 *   fake_steamclient.c      the implementation class + its vtable, in a
 *                           separate module, like steamclient64.dll
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* msvcrt's strstr is not linked into this PE; keep a local one. */
static const char *find_sub(const char *hay, const char *needle)
{
    if (!hay || !needle)
        return 0;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n)
            return hay;
    }
    return 0;
}

static HMODULE g_sc;
static void *(*sc_iface)(void);
static unsigned long long (*sc_create_max)(void);
static int (*sc_setmax)(void);
static void *(*sc_friends_iface)(void);
static unsigned long long (*sc_rp_friend)(void);
static const char *(*sc_last_value)(void);
static int (*sc_slots_calls)(void);
static int (*sc_filter_calls)(void);
static int (*sc_list_calls)(void);
static int (*sc_invite_calls)(void);
static int (*sc_memberdata_calls)(void);

/* steamclient is what actually owns the interface object and its vtable */
static void load_sc(void)
{
    if (g_sc)
        return;
    g_sc = LoadLibraryA("steamclient64.dll");
    if (!g_sc)
        return;
    sc_iface      = (void *(*)(void))GetProcAddress(g_sc, "nioh3p_sc_iface");
    sc_create_max = (unsigned long long (*)(void))GetProcAddress(g_sc, "nioh3p_sc_create_max");
    sc_setmax     = (int (*)(void))GetProcAddress(g_sc, "nioh3p_sc_setmax");
    sc_friends_iface = (void *(*)(void))GetProcAddress(g_sc, "nioh3p_sc_friends_iface");
    sc_rp_friend  = (unsigned long long (*)(void))GetProcAddress(g_sc, "nioh3p_sc_rp_friend");
    sc_last_value = (const char *(*)(void))GetProcAddress(g_sc, "nioh3p_sc_last_value");
    sc_slots_calls = (int (*)(void))GetProcAddress(g_sc, "nioh3p_sc_slots_calls");
    sc_filter_calls = (int (*)(void))GetProcAddress(g_sc, "nioh3p_sc_filter_calls");
    sc_list_calls = (int (*)(void))GetProcAddress(g_sc, "nioh3p_sc_list_calls");
    sc_invite_calls = (int (*)(void))GetProcAddress(g_sc, "nioh3p_sc_invite_calls");
    sc_memberdata_calls = (int (*)(void))GetProcAddress(g_sc, "nioh3p_sc_memberdata_calls");
}

void *__stdcall SteamInternal_FindOrCreateUserInterface(void *user, const char *version)
{
    (void)user;
    load_sc();
    if (version && version[0] && find_sub(version, "Friends"))
        return sc_friends_iface ? sc_friends_iface() : 0;
    return sc_iface ? sc_iface() : 0;
}

/* read back what got through to the service */
__declspec(dllexport) unsigned long long nioh3p_test_create_max(void)
{
    load_sc();
    return sc_create_max ? sc_create_max() : 0;
}

/* CCallResult's slot 0 matches the incoming handle before consuming it.
 * The layout through call (+0x10) matches the game's callback object. */
static void *g_cb_vtable[4];
static struct {
    void **vtable;
    uint8_t flags;
    int callback_id;
    uint64_t call;
} g_cb_object;
static int g_cb_run_calls;
static uint8_t g_cb_failed;

static void __stdcall test_callback_run(void *self, void *param, uint8_t failed, uint64_t call)
{
    (void)self;
    (void)param;
    if (!g_cb_object.call || call != g_cb_object.call)
        return;
    g_cb_object.call = 0;
    g_cb_failed = failed;
    g_cb_run_calls++;
}

void __stdcall SteamAPI_RegisterCallback(void *cb, int cb_id)
{
    (void)cb;
    (void)cb_id;
}

void __stdcall SteamAPI_RegisterCallResult(void *cb, uint64_t call)
{
    (void)cb;
    (void)call;
}

__declspec(dllexport) void *nioh3p_test_callback(void)
{
    g_cb_vtable[0] = (void *)test_callback_run;
    g_cb_object.vtable = g_cb_vtable;
    g_cb_object.call = 0xfedcba9812345678ull;
    return &g_cb_object;
}
__declspec(dllexport) int nioh3p_test_cb_runs(void) { return g_cb_run_calls; }
__declspec(dllexport) uint8_t nioh3p_test_cb_failed(void) { return g_cb_failed; }
__declspec(dllexport) int nioh3p_test_slots_calls(void)
{
    load_sc();
    return sc_slots_calls ? sc_slots_calls() : 0;
}
__declspec(dllexport) const char *nioh3p_test_last_value(void)
{
    load_sc();
    return sc_last_value ? sc_last_value() : "";
}
__declspec(dllexport) int nioh3p_test_filter_calls(void)
{
    load_sc();
    return sc_filter_calls ? sc_filter_calls() : 0;
}
__declspec(dllexport) int nioh3p_test_list_calls(void)
{
    load_sc();
    return sc_list_calls ? sc_list_calls() : 0;
}
__declspec(dllexport) unsigned long long nioh3p_test_rp_friend(void)
{
    load_sc();
    return sc_rp_friend ? sc_rp_friend() : 0;
}

__declspec(dllexport) int nioh3p_test_setmax(void)
{
    load_sc();
    return sc_setmax ? sc_setmax() : 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)self;
    (void)reserved;
    (void)reason;      /* resolved lazily: see sc_init() in fake_steamclient.c */
    return TRUE;
}
