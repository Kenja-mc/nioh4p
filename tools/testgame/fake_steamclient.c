/*
 * Fake steamclient64.dll: the "implementation class" behind the matchmaking
 * interface. Its vtable holds *its own* functions (not the flat API entries of
 * steam_api64.dll), which is why the proxy has to find the slots by decoding the
 * thunks and then patch the object it was handed.
 *
 * Slot indices match the offsets the real thunks use: 0x68/8 = 13,
 * 0xf8/8 = 31.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#define VT_SLOTS 48
#define SLOT_CREATE 13
#define SLOT_SETMAX 31
#define SLOT_MEMBERS 17
#define SLOT_REQUESTLIST 4
#define SLOT_STRINGFILTER 5
#define SLOT_BYINDEX 12
#define SLOT_OWNER 35
#define SLOT_SLOTSFILTER 8
#define SLOT_SETDATA 20
#define SLOT_INVITE 16
#define SLOT_MEMBERDATA 25
#define FSLOT_RICHP 45
#define FSLOT_BYINDEX 4

static unsigned long long g_last_create_max;
static int g_last_setmax;
static int g_create_calls;
static int g_setmax_calls;

static uint64_t sc_create_lobby(void *self, int lobby_type, int cMaxMembers)
{
    (void)self;
    (void)lobby_type;
    g_last_create_max = (unsigned long long)(unsigned)cMaxMembers;
    g_create_calls++;
    return 1;
}

static uint8_t sc_set_lobby_member_limit(void *self, uint64_t lobby, int cMaxMembers)
{
    (void)self;
    if (lobby != 0x0180000103C821A2ull)
        return 0;
    g_last_setmax = cMaxMembers;
    g_setmax_calls++;
    return 1;
}

static int g_members_calls;

static int sc_get_num_lobby_members(void *self, uint64_t lobby)
{
    (void)self;
    (void)lobby;
    g_members_calls++;
    return 2;                  /* host + companion, as the real service would */
}

static uint64_t g_owner_calls_ret = 0x0110000103C821A2ull;   /* a SteamID64 */

static uint64_t *sc_get_lobby_owner(void *self, uint64_t *out, uint64_t lobby)
{
    (void)self;
    *out = lobby == 0x0180000103C821A2ull ? g_owner_calls_ret : 0;
    return out;
}

static uint64_t *sc_get_lobby_member_by_index(void *self, uint64_t *out, uint64_t lobby, int index)
{
    (void)self;
    *out = lobby == 0x0180000103C821A2ull && index == 1 ? g_owner_calls_ret : 0;
    return out;
}

static char g_last_value[128];
static int g_slots_calls;

static void sc_add_slots_filter(void *self, int slots)
{
    (void)self; (void)slots;
    g_slots_calls++;
}

static int sc_set_lobby_data(void *self, uint64_t lobby, const char *key, const char *value)
{
    (void)self; (void)lobby; (void)key;
    unsigned i = 0;
    if (value)
        while (value[i] && i + 1 < sizeof g_last_value) { g_last_value[i] = value[i]; i++; }
    g_last_value[i] = 0;
    return 1;
}

const char *__stdcall nioh3p_sc_last_value(void) { return g_last_value; }
int __stdcall nioh3p_sc_slots_calls(void) { return g_slots_calls; }

static int g_filter_calls;
static int g_list_calls;

static unsigned long long sc_request_lobby_list(void *self)
{
    (void)self;
    g_list_calls++;
    return 0x1234ull;                       /* a SteamAPICall_t */
}

static void sc_add_string_filter(void *self, const char *key, const char *value, int cmp)
{
    (void)self; (void)key; (void)value; (void)cmp;
    g_filter_calls++;
}

static uint64_t *sc_get_lobby_by_index(void *self, uint64_t *out, int index)
{
    (void)self;
    *out = index == 0 ? 0x0180000103C821A2ull : 0;
    return out;
}

static int g_invite_calls;
static int g_memberdata_calls;

static int sc_invite_user_to_lobby(void *self, uint64_t lobby, uint64_t invitee)
{
    (void)self; (void)lobby; (void)invitee;
    g_invite_calls++;
    return 1;
}

static int sc_set_lobby_member_data(void *self, uint64_t lobby, const char *key, const char *value)
{
    (void)self; (void)lobby; (void)key; (void)value;
    g_memberdata_calls++;
    return 1;
}

/* a second interface of a different type, to exercise the generic path */
static void *g_friends_vtable[VT_SLOTS];
static void *g_friends_object[8];
static uint64_t g_rp_friend;

static const char *sc_get_friend_rich_presence(void *self, uint64_t friend_id, const char *key)
{
    (void)self; (void)key;
    g_rp_friend = friend_id;
    return "nioh-session=1";
}

static uint64_t *sc_get_friend_by_index(void *self, uint64_t *out, int index, int flags)
{
    (void)self;
    *out = index == 1 && flags == 4 ? g_owner_calls_ret : 0;
    return out;
}

void *__stdcall nioh3p_sc_friends_iface(void)
{
    g_friends_vtable[FSLOT_RICHP] = (void *)sc_get_friend_rich_presence;
    g_friends_vtable[FSLOT_BYINDEX] = (void *)sc_get_friend_by_index;
    g_friends_object[0] = g_friends_vtable;
    return g_friends_object;
}

uint64_t __stdcall nioh3p_sc_rp_friend(void) { return g_rp_friend; }
int __stdcall nioh3p_sc_filter_calls(void) { return g_filter_calls; }
int __stdcall nioh3p_sc_list_calls(void) { return g_list_calls; }
int __stdcall nioh3p_sc_invite_calls(void) { return g_invite_calls; }
int __stdcall nioh3p_sc_memberdata_calls(void) { return g_memberdata_calls; }

static void *g_vtable[VT_SLOTS];
static void *g_iface[4];
static int g_init;

/* Initialised on first use, not from DllMain: the proxy's test harness loads
 * this module from inside another DLL's attach, and Wine does not run the entry
 * point of a module loaded that way. */
static void sc_init(void)
{
    if (g_init)
        return;
    g_init = 1;
    g_vtable[SLOT_CREATE] = (void *)sc_create_lobby;
    g_vtable[SLOT_SETMAX] = (void *)sc_set_lobby_member_limit;
    g_vtable[SLOT_MEMBERS] = (void *)sc_get_num_lobby_members;
    g_vtable[SLOT_OWNER] = (void *)sc_get_lobby_owner;
    g_vtable[18] = (void *)sc_get_lobby_member_by_index;
    g_vtable[SLOT_SLOTSFILTER] = (void *)sc_add_slots_filter;
    g_vtable[SLOT_SETDATA] = (void *)sc_set_lobby_data;
    g_vtable[SLOT_REQUESTLIST] = (void *)sc_request_lobby_list;
    g_vtable[SLOT_STRINGFILTER] = (void *)sc_add_string_filter;
    g_vtable[SLOT_BYINDEX] = (void *)sc_get_lobby_by_index;
    g_vtable[SLOT_INVITE] = (void *)sc_invite_user_to_lobby;
    g_vtable[SLOT_MEMBERDATA] = (void *)sc_set_lobby_member_data;
    g_iface[0] = g_vtable;
}

__declspec(dllexport) void *nioh3p_sc_iface(void) { sc_init(); return g_iface; }
__declspec(dllexport) unsigned long long nioh3p_sc_create_max(void) { return g_last_create_max; }
__declspec(dllexport) int nioh3p_sc_setmax(void) { return g_last_setmax; }
__declspec(dllexport) int nioh3p_sc_calls(void) { return g_create_calls + g_setmax_calls; }
__declspec(dllexport) int nioh3p_sc_members_calls(void) { return g_members_calls; }
__declspec(dllexport) void nioh3p_sc_reset(void)
{
    g_last_create_max = 0;
    g_last_setmax = 0;
    g_create_calls = 0;
    g_setmax_calls = 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)self;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        sc_init();
    return TRUE;
}
