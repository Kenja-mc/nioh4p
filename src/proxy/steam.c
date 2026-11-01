/* nioh3p core: steam (split from nioh3p_proxy.c) */
#include "nioh3p_internal.h"

/* ------------------------------------------------------- Steam matchmaking */

/* The matchmaking interface is selected by exact version string and its
 * SteamMatchMaking009 slots are bound explicitly below. */
typedef uint64_t (EOS_CALL *PFN_SteamCreateLobby)(void *, int, int);
typedef uint8_t  (EOS_CALL *PFN_SteamSetLobbyMemberLimit)(void *, uint64_t, int);
typedef void *   (EOS_CALL *PFN_FindOrCreate)(void *, const char *);
typedef int         (EOS_CALL *PFN_SteamSetLobbyData)(void *, uint64_t, const char *, const char *);
typedef uint64_t    (EOS_CALL *PFN_SteamJoinLobby)(void *, uint64_t);
typedef void        (EOS_CALL *PFN_SteamLeaveLobby)(void *, uint64_t);
static void *EOS_CALL det_FindOrCreateInterface(void *, const char *);

static PFN_FindOrCreate orig_FindOrCreate;

/* Trace-only hook signatures and their originals: research build only. */
#ifdef NIOH3P_RESEARCH
typedef const char *(EOS_CALL *PFN_SteamGetLobbyData)(void *, uint64_t, const char *);
typedef int         (EOS_CALL *PFN_SteamGetNumLobbyMembers)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamGetLobbyMemberLimit)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamSendLobbyChatMsg)(void *, uint64_t, const void *, int);
typedef int         (EOS_CALL *PFN_SteamRequestLobbyData)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamGetLobbyDataCount)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamGetLobbyDataByIndex)(void *, uint64_t, int, char *, int, char *, int);
/* MSVC returns CSteamID through a caller-owned buffer after `this`, unlike
 * the flat exports that unwrap that buffer into a uint64_t return value. */
typedef uint64_t *  (EOS_CALL *PFN_SteamGetLobbyOwner)(void *, uint64_t *, uint64_t);
typedef uint64_t *  (EOS_CALL *PFN_SteamGetLobbyMemberByIndex)(void *, uint64_t *, uint64_t, int);
typedef void        (EOS_CALL *PFN_SteamRegisterCallback)(void *, int);
typedef void        (EOS_CALL *PFN_SteamRegisterCallResult)(void *, uint64_t);
typedef void        (EOS_CALL *PFN_CallbackRun)(void *, void *, uint8_t, uint64_t);

static void *orig_RegisterCallback;
static void *orig_RegisterCallResult;
static void *orig_ExitProcess;
static void *orig_TerminateProcess;

/* Identify who ends the process: log the return address so the caller module
 * (nioh.exe vs the emu client dll) shows up in the log. */
static void __cdecl det_ExitProcess(unsigned int code)
{
    void *ret = __builtin_return_address(0);
    LOG("EXIT: ExitProcess(%u) called from %p - process ending", code, ret);
    ((void (__cdecl *)(unsigned int))orig_ExitProcess)(code);
    for (;;) Sleep(1000);   /* original never returns */
}

static int __stdcall det_TerminateProcess(void *h, unsigned int code)
{
    void *ret = __builtin_return_address(0);
    LOG("EXIT: TerminateProcess(handle=%p, %u) from %p", h, code, ret);
    return ((int (__stdcall *)(void *, unsigned int))orig_TerminateProcess)(h, code);
}
#endif

static uint64_t EOS_CALL det_SteamCreateLobby(void *, int, int);
static uint8_t EOS_CALL det_SteamSetLobbyMemberLimit(void *, uint64_t, int);
static int EOS_CALL det_SteamSetLobbyData(void *, uint64_t, const char *, const char *);
static uint64_t EOS_CALL det_SteamJoinLobby(void *, uint64_t);
static void EOS_CALL det_SteamLeaveLobby(void *, uint64_t);
static void EOS_CALL det_SteamAddStringFilter(void *, const char *, const char *, int);
static void EOS_CALL det_SteamAddNumericalFilter(void *, const char *, int, int);
static void EOS_CALL det_SteamAddSlotsFilter(void *, int);

#ifdef NIOH3P_RESEARCH
static const char *EOS_CALL det_SteamGetLobbyData(void *, uint64_t, const char *);
static int EOS_CALL det_SteamGetNumLobbyMembers(void *, uint64_t);
static int EOS_CALL det_SteamGetLobbyMemberLimit(void *, uint64_t);
static int EOS_CALL det_SteamSendLobbyChatMsg(void *, uint64_t, const void *, int);
static int EOS_CALL det_SteamRequestLobbyData(void *, uint64_t);
static int EOS_CALL det_SteamGetLobbyDataCount(void *, uint64_t);
static int EOS_CALL det_SteamGetLobbyDataByIndex(void *, uint64_t, int, char *, int, char *, int);
static uint64_t *EOS_CALL det_SteamGetLobbyOwner(void *, uint64_t *, uint64_t);
static uint64_t *EOS_CALL det_SteamGetLobbyMemberByIndex(void *, uint64_t *, uint64_t, int);
static int EOS_CALL det_SteamInviteUserToLobby(void *, uint64_t, uint64_t);
static const char *EOS_CALL det_SteamGetLobbyMemberData(void *, uint64_t, uint64_t, const char *);
static int EOS_CALL det_SteamSetLobbyMemberData(void *, uint64_t, const char *, const char *);
static uint64_t EOS_CALL det_SteamRequestLobbyList(void *);
static void EOS_CALL det_SteamAddNearValueFilter(void *, const char *, int);
static void EOS_CALL det_SteamAddDistanceFilter(void *, int);
static void EOS_CALL det_SteamAddResultCountFilter(void *, int);
static void EOS_CALL det_SteamAddCompatibleFilter(void *, uint64_t);
static uint64_t *EOS_CALL det_SteamGetLobbyByIndex(void *, uint64_t *, int);
static int EOS_CALL det_FriendCount(void *, int);
static uint64_t *EOS_CALL det_FriendByIndex(void *, uint64_t *, int, int);
static int EOS_CALL det_FriendGamePlayed(void *, uint64_t, void *);
static int EOS_CALL det_FriendState(void *, uint64_t);
static const char *EOS_CALL det_FriendRichPresence(void *, uint64_t, const char *);
static int EOS_CALL det_FriendRpKeyCount(void *, uint64_t);
static const char *EOS_CALL det_FriendRpKeyByIndex(void *, uint64_t, int);
static const char *EOS_CALL det_FriendPersonaName(void *, uint64_t);
static int EOS_CALL det_P2PSend(void *, uint64_t, const void *, unsigned, int, int);
static int EOS_CALL det_P2PAvailable(void *, unsigned *, int);
static int EOS_CALL det_P2PRead(void *, void *, unsigned, unsigned *, uint64_t *, int);
static int EOS_CALL det_P2PAccept(void *, uint64_t);
static int EOS_CALL det_P2PClose(void *, uint64_t);
static int EOS_CALL det_P2PState(void *, uint64_t, void *);
static int EOS_CALL det_SockConnectP2P(void *, const void *, int, int, const void *);
static int EOS_CALL det_SockCreateListen(void *, int, int, const void *);
static int EOS_CALL det_SockSend(void *, int, const void *, unsigned, int, int64_t *);
static int EOS_CALL det_SockClose(void *, int, int, const char *, int);
static void EOS_CALL det_RegisterCallback(void *, int);
static void EOS_CALL det_RegisterCallResult(void *, uint64_t);
static void EOS_CALL det_CallbackRun(void *, void *, uint8_t, uint64_t);
#endif

/* Slots below use zero-based vtable indices from the versioned Steam headers
 * and the game-image call-site map. SteamHook stores them one-based so zero
 * remains the unresolved sentinel. */
/* Behaviour-changing hooks: installed in both modes. The numerical filter
 * changes nothing, but it sits here because player mode needs its keys in the
 * log (filter_key_note) and a trace-only detour would never see them. */
#define STEAM_HOOK(name, target, index) \
    { name, (void *)(uintptr_t)target, (index) + 1, 0, 0, 0 }
/* Observational hooks: not installed at all in player mode, so the game's
 * network path never enters the proxy. The player build keeps the array entry
 * (the behaviour hooks after it are addressed by position) but with neither a
 * detour nor a vtable slot, so the entry is inert and its detour is not
 * compiled in. */
#ifdef NIOH3P_RESEARCH
#define STEAM_TRACE_HOOK(name, target, index) \
    { name, (void *)(uintptr_t)target, (index) + 1, 1, 0, 0 }
#else
#define STEAM_TRACE_HOOK(name, target, index) \
    { name, 0, 0, 1, 0, 0 }
#endif

/* SteamMatchMaking009 */
SteamHook g_steam[] = {
    STEAM_HOOK("CreateLobby", det_SteamCreateLobby, 13),
    STEAM_HOOK("SetLobbyMemberLimit", det_SteamSetLobbyMemberLimit, 31),
    STEAM_HOOK("SetLobbyData", det_SteamSetLobbyData, 20),
    STEAM_TRACE_HOOK("GetLobbyData", det_SteamGetLobbyData, 19),
    STEAM_TRACE_HOOK("GetNumLobbyMembers", det_SteamGetNumLobbyMembers, 17),
    STEAM_TRACE_HOOK("GetLobbyMemberLimit", det_SteamGetLobbyMemberLimit, 32),
    STEAM_TRACE_HOOK("SendLobbyChatMsg", det_SteamSendLobbyChatMsg, 26),
    STEAM_HOOK("JoinLobby", det_SteamJoinLobby, 14),
    STEAM_TRACE_HOOK("RequestLobbyData", det_SteamRequestLobbyData, 28),
    STEAM_TRACE_HOOK("GetLobbyDataCount", det_SteamGetLobbyDataCount, 21),
    STEAM_TRACE_HOOK("GetLobbyDataByIndex", det_SteamGetLobbyDataByIndex, 22),
    STEAM_HOOK("LeaveLobby", det_SteamLeaveLobby, 15),
    STEAM_TRACE_HOOK("GetLobbyOwner", det_SteamGetLobbyOwner, 35),
    STEAM_TRACE_HOOK("GetLobbyMemberByIndex", det_SteamGetLobbyMemberByIndex, 18),
    STEAM_TRACE_HOOK("InviteUserToLobby", det_SteamInviteUserToLobby, 16),
    STEAM_TRACE_HOOK("GetLobbyMemberData", det_SteamGetLobbyMemberData, 24),
    STEAM_TRACE_HOOK("SetLobbyMemberData", det_SteamSetLobbyMemberData, 25),
    STEAM_TRACE_HOOK("RequestLobbyList", det_SteamRequestLobbyList, 4),
    STEAM_HOOK("AddRequestLobbyListStringFilter", det_SteamAddStringFilter, 5),
    STEAM_HOOK("AddRequestLobbyListNumericalFilter", det_SteamAddNumericalFilter, 6),
    STEAM_TRACE_HOOK("AddRequestLobbyListNearValueFilter", det_SteamAddNearValueFilter, 7),
    STEAM_HOOK("AddRequestLobbyListFilterSlotsAvailable", det_SteamAddSlotsFilter, 8),
    STEAM_TRACE_HOOK("AddRequestLobbyListDistanceFilter", det_SteamAddDistanceFilter, 9),
    STEAM_TRACE_HOOK("AddRequestLobbyListResultCountFilter", det_SteamAddResultCountFilter, 10),
    STEAM_TRACE_HOOK("AddRequestLobbyListCompatibleMembersFilter", det_SteamAddCompatibleFilter, 11),
    STEAM_TRACE_HOOK("GetLobbyByIndex", det_SteamGetLobbyByIndex, 12),
};
#define NSTEAM (int)(sizeof g_steam / sizeof g_steam[0])

/* SteamFriends017, SteamNetworking006 and SteamNetworkingSockets009 are
 * observation only: trace=1 in the research build, absent in the player build. */
#ifdef NIOH3P_RESEARCH
/* SteamFriends017 */
enum { FR_COUNT, FR_BYINDEX, FR_GAMEPLAYED, FR_STATE, FR_RICHP, FR_RPCOUNT, FR_RPKEY, FR_NAME, NFRIENDS };
static SteamHook g_friends[] = {
    STEAM_TRACE_HOOK("GetFriendCount", det_FriendCount, 3),
    STEAM_TRACE_HOOK("GetFriendByIndex", det_FriendByIndex, 4),
    STEAM_TRACE_HOOK("GetFriendGamePlayed", det_FriendGamePlayed, 8),
    STEAM_TRACE_HOOK("GetFriendPersonaState", det_FriendState, 6),
    STEAM_TRACE_HOOK("GetFriendRichPresence", det_FriendRichPresence, 45),
    STEAM_TRACE_HOOK("GetFriendRichPresenceKeyCount", det_FriendRpKeyCount, 46),
    STEAM_TRACE_HOOK("GetFriendRichPresenceKeyByIndex", det_FriendRpKeyByIndex, 47),
    STEAM_TRACE_HOOK("GetFriendPersonaName", det_FriendPersonaName, 7),
};

/* SteamNetworking006 */
enum { P_SEND, P_AVAIL, P_READ, P_ACCEPT, P_CLOSE, P_STATE, NNET };
static SteamHook g_networking[] = {
    STEAM_TRACE_HOOK("SendP2PPacket", det_P2PSend, 0),
    STEAM_TRACE_HOOK("IsP2PPacketAvailable", det_P2PAvailable, 1),
    STEAM_TRACE_HOOK("ReadP2PPacket", det_P2PRead, 2),
    STEAM_TRACE_HOOK("AcceptP2PSessionWithUser", det_P2PAccept, 3),
    STEAM_TRACE_HOOK("CloseP2PSessionWithUser", det_P2PClose, 4),
    STEAM_TRACE_HOOK("GetP2PSessionState", det_P2PState, 6),
};

/* SteamNetworkingSockets009 */
enum { S_CONNECT, S_LISTEN, S_SEND, S_CLOSE, NSOCK };
static SteamHook g_sockets[] = {
    STEAM_TRACE_HOOK("ConnectP2P", det_SockConnectP2P, 3),
    STEAM_TRACE_HOOK("CreateListenSocketP2P", det_SockCreateListen, 2),
    STEAM_TRACE_HOOK("SendMessageToConnection", det_SockSend, 11),
    STEAM_TRACE_HOOK("CloseConnection", det_SockClose, 5),
};
#endif

/* How many vtable methods this mode configures, for the startup mode line. */
int steam_method_count(void)
{
#ifdef NIOH3P_RESEARCH
    SteamHook *tbls[] = { g_steam, g_friends, g_networking, g_sockets };
    const int sizes[] = { NSTEAM, NFRIENDS, NNET, NSOCK };
    int n = 0;
    for (int t = 0; t < (int)(sizeof tbls / sizeof tbls[0]); t++)
        for (int i = 0; i < sizes[t]; i++)
            if (tbls[t][i].slot_index && (g_cfg.trace || !tbls[t][i].trace_only))
                n++;
    return n;
#else
    int n = 0;
    for (int i = 0; i < NSTEAM; i++)
        if (g_steam[i].slot_index && !g_steam[i].trace_only)
            n++;
    return n;               /* identical to the research build at trace=0 */
#endif
}

static void steam_apply_table(void *iface, SteamHook *tbl, int n);

int g_steam_ready;

/* Called with the object returned for a supported interface version; patches
 * that version's configured methods in its vtable. */
static void steam_apply_table(void *iface, SteamHook *tbl, int n)
{
    if (!iface || !readable(iface, sizeof(void *)))
        return;
    void **vt = *(void ***)iface;
    if (!readable(vt, 8))
        return;
    for (int i = 0; i < n; i++) {
        SteamHook *h = &tbl[i];
        if (!h->slot_index || (h->trace_only && !g_cfg.trace))
            continue;
        unsigned index = h->slot_index - 1;
        void **slot = vt + index;
        if (!readable(slot, sizeof(void *)))
            continue;
        if (*slot == h->det) {
            h->slot = slot;
            continue;
        }
        if (!executable_ptr(*slot)) {
            LOG("steam: vtable[%u] (%s) target %p is not executable code; not patched",
                index, h->name, *slot);
            continue;
        }
        DWORD old;
        if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
            LOG("steam: VirtualProtect(vtable[%u]) failed", index);
            continue;
        }
        void *prev = *slot;
        /* The detour dereferences h->orig: it must be visible first. */
        if (!h->orig)
            h->orig = prev;
        __asm__ volatile("" ::: "memory");
        *slot = h->det;
        VirtualProtect(slot, sizeof(void *), old, &old);
        h->slot = slot;
        LOG("steam: vtable[%u] (offset 0x%x, %s) %p -> %p",
            index, index * 8, h->name, prev, h->det);
    }
}

int steam_repair(void)
{
    int fixed = 0;
    for (int i = 0; i < NSTEAM; i++) {
        SteamHook *h = &g_steam[i];
        if (!h->slot || *h->slot == h->det)
            continue;
        DWORD old;
        if (VirtualProtect(h->slot, sizeof(void *), PAGE_READWRITE, &old)) {
            *h->slot = h->det;
            VirtualProtect(h->slot, sizeof(void *), old, &old);
            fixed++;
        }
    }
    return fixed;
}

/* CreateLobby returns only an API call handle, never proof that this process
 * owns a lobby. The handle is kept as a publication hint; ownership is
 * confirmed only when our own SetLobbyData for the rule key succeeds (Steam
 * rejects lobby data writes from non-owners). The hint is rules state, so it
 * is recorded through the rules lock like every other transition. */
void rules_note_create_request(void);
static uint64_t EOS_CALL det_SteamCreateLobby(void *self, int lobby_type, int max_members)
{
    SITE_NOTE("SteamCreateLobby");
    LOG("Steam_ISteamMatchmaking::CreateLobby: type=%d maxMembers=%d", lobby_type, max_members);
    if (g_cfg.max_players && max_members > 0 && max_members < (int)g_cfg.max_players) {
        LOG("Steam CreateLobby: raising cMaxMembers %d -> %u", max_members, g_cfg.max_players);
        max_members = (int)g_cfg.max_players;
    }
#ifdef NIOH3P_RESEARCH
    if (g_cfg.trace)
        log_backtrace("Steam_ISteamMatchmaking::CreateLobby");
#endif
    PFN_SteamCreateLobby o = (PFN_SteamCreateLobby)g_steam[0].orig;
    uint64_t call = o ? o(self, lobby_type, max_members) : 0;
    /* Nonzero is an API call handle, not a created lobby; it only enables the
     * publication attempt. A successful rule-key write records ownership. */
    if (call)
        rules_note_create_request();
    LOG("Steam CreateLobby -> call %llu", (unsigned long long)call);
    return call;
}

static uint8_t EOS_CALL det_SteamSetLobbyMemberLimit(void *self, uint64_t lobby, int max_members)
{
    SITE_NOTE("SteamSetLobbyMemberLimit");
    LOG("Steam_ISteamMatchmaking::SetLobbyMemberLimit: lobby=%llu maxMembers=%d",
        (unsigned long long)lobby, max_members);
    if (g_cfg.max_players && max_members > 0 && max_members < (int)g_cfg.max_players) {
        LOG("Steam SetLobbyMemberLimit: raising %d -> %u", max_members, g_cfg.max_players);
        max_members = (int)g_cfg.max_players;
    }
#ifdef NIOH3P_RESEARCH
    if (g_cfg.trace)
        log_backtrace("Steam_ISteamMatchmaking::SetLobbyMemberLimit");
#endif
    PFN_SteamSetLobbyMemberLimit o = (PFN_SteamSetLobbyMemberLimit)g_steam[1].orig;
    return o ? o(self, lobby, max_members) : 0;
}

void install_game_metadata_guard(void);
/* Lobby rules live after the game-protocol constants; these are the only
 * Steam-side entry points that need them. */
void rules_publish(void *self, uint64_t lobby);
void rules_join_lobby(void *self, uint64_t lobby);
void rules_left_lobby(uint64_t lobby);

/* Lobby *data* is what a joining client filters on: if the host advertises a
 * two-slot session, a third player's client never lists it as joinable. */
static int EOS_CALL det_SteamSetLobbyData(void *self, uint64_t lobby, const char *key, const char *value)
{
    SITE_NOTE("SteamSetLobbyData");
    /* CreateLobby only returns an API call handle; the lobby id first shows up
     * here, when the host publishes its data. One line per lobby. */
    static volatile uint64_t s_logged_lobby;
    if (lobby && lobby != s_logged_lobby) {
        s_logged_lobby = lobby;
        LOG("Steam host lobby id=%llu", (unsigned long long)lobby);
    }
    if (__builtin_return_address(0) == g_base + 0x5ffe52)
        install_game_metadata_guard();
    const char *use = value;
    if (g_cfg.force_unlock && key && value && strstr_a(key, "lock_status") &&
        strstr_a(value, "lock") && !strstr_a(value, "unlock")) {
        use = "ktol_session_unlock";
        LOG("Steam SetLobbyData: key=\"%.40s\" value \"%.40s\" -> \"ktol_session_unlock\" (force_unlock=1)",
            key, value);
    } else if (g_cfg.trace) {
        LOG("Steam SetLobbyData: key=\"%s\" value=\"%s\"",
            key ? key : "(null)", value ? value : "(null)");
    }
    PFN_SteamSetLobbyData o = (PFN_SteamSetLobbyData)g_steam[2].orig;
    int r = o ? o(self, lobby, key, use) : 0;
    /* The host's own keys are already in place; publish our rule string once
     * per lobby after the native write so a joining client sees both. */
    rules_publish(self, lobby);
    return r;
}

#ifdef NIOH3P_RESEARCH
static const char *EOS_CALL det_SteamGetLobbyData(void *self, uint64_t lobby, const char *key)
{
    SITE_NOTE("SteamGetLobbyData");
    PFN_SteamGetLobbyData o = (PFN_SteamGetLobbyData)g_steam[3].orig;
    const char *v = o ? o(self, lobby, key) : 0;
    LOG("Steam GetLobbyData: key=\"%s\" -> \"%s\"", key ? key : "(null)", v ? v : "(null)");
    return v;
}

static int EOS_CALL det_SteamGetNumLobbyMembers(void *self, uint64_t lobby)
{
    SITE_NOTE("SteamGetNumLobbyMembers");
    PFN_SteamGetNumLobbyMembers o = (PFN_SteamGetNumLobbyMembers)g_steam[4].orig;
    int n = o ? o(self, lobby) : 0;
    LOG("Steam GetNumLobbyMembers -> %d", n);
    return n;
}

static int EOS_CALL det_SteamGetLobbyMemberLimit(void *self, uint64_t lobby)
{
    SITE_NOTE("SteamGetLobbyMemberLimit");
    PFN_SteamGetLobbyMemberLimit o = (PFN_SteamGetLobbyMemberLimit)g_steam[5].orig;
    int n = o ? o(self, lobby) : 0;
    LOG("Steam GetLobbyMemberLimit -> %d", n);
    return n;
}
#endif

static uint64_t EOS_CALL det_SteamJoinLobby(void *self, uint64_t lobby)
{
    SITE_NOTE("SteamJoinLobby");
    LOG("Steam JoinLobby: lobby=%llu", (unsigned long long)lobby);
    PFN_SteamJoinLobby o = (PFN_SteamJoinLobby)g_steam[7].orig;
    uint64_t call = o ? o(self, lobby) : 0;
    /* Without a forward pointer or a call handle the join never reached Steam,
     * so there is no client lobby to record; the previous authority stands. */
    if (lobby && call)
        rules_join_lobby(self, lobby);
    else if (lobby)
        LOG("Steam JoinLobby: lobby=%llu got no call handle; rule authority unchanged",
            (unsigned long long)lobby);
    LOG("Steam JoinLobby -> call %llu", (unsigned long long)call);
#ifdef NIOH3P_RESEARCH
    if (g_cfg.trace)
        log_backtrace("Steam JoinLobby");
#endif
    return call;
}

#ifdef NIOH3P_RESEARCH
static int EOS_CALL det_SteamRequestLobbyData(void *self, uint64_t lobby)
{
    SITE_NOTE("SteamRequestLobbyData");
    PFN_SteamRequestLobbyData o = (PFN_SteamRequestLobbyData)g_steam[8].orig;
    int r = o ? o(self, lobby) : 0;
    LOG("Steam RequestLobbyData: lobby=%llu -> %d", (unsigned long long)lobby, r);
    return r;
}

static int EOS_CALL det_SteamGetLobbyDataCount(void *self, uint64_t lobby)
{
    SITE_NOTE("SteamGetLobbyDataCount");
    PFN_SteamGetLobbyDataCount o = (PFN_SteamGetLobbyDataCount)g_steam[9].orig;
    int n = o ? o(self, lobby) : 0;
    LOG("Steam GetLobbyDataCount -> %d", n);
    return n;
}

static int EOS_CALL det_SteamGetLobbyDataByIndex(void *self, uint64_t lobby, int index,
                                                 char *key, int key_len, char *value, int value_len)
{
    SITE_NOTE("SteamGetLobbyDataByIndex");
    PFN_SteamGetLobbyDataByIndex o = (PFN_SteamGetLobbyDataByIndex)g_steam[10].orig;
    int r = o ? o(self, lobby, index, key, key_len, value, value_len) : 0;
    LOG("Steam GetLobbyDataByIndex[%d] -> key=\"%s\" value=\"%s\"",
        index, key ? key : "", value ? value : "");
    return r;
}

static uint64_t *EOS_CALL det_SteamGetLobbyOwner(void *self, uint64_t *out, uint64_t lobby)
{
    SITE_NOTE("SteamGetLobbyOwner");
    PFN_SteamGetLobbyOwner o = (PFN_SteamGetLobbyOwner)g_steam[12].orig;
    /* Same rule as det_SteamCreateLobby: without a forward pointer or a
     * patched slot there is nothing to forward to, and the SteamID has to be
     * zeroed by the caller, not read out of a NULL return. */
    uint64_t *result = (o && g_steam[12].slot) ? o(self, out, lobby) : 0;
    LOG("Steam GetLobbyOwner -> %llu", result ? (unsigned long long)*result : 0);
    return result;
}

static uint64_t *EOS_CALL det_SteamGetLobbyMemberByIndex(void *self, uint64_t *out, uint64_t lobby, int index)
{
    SITE_NOTE("SteamGetLobbyMemberByIndex");
    PFN_SteamGetLobbyMemberByIndex o = (PFN_SteamGetLobbyMemberByIndex)g_steam[13].orig;
    uint64_t *result = (o && g_steam[13].slot) ? o(self, out, lobby, index) : 0;
    LOG("Steam GetLobbyMemberByIndex[%d] -> %llu", index,
        result ? (unsigned long long)*result : 0);
    return result;
}
#endif

static void EOS_CALL det_SteamLeaveLobby(void *self, uint64_t lobby)
{
    SITE_NOTE("SteamLeaveLobby");
    LOG("Steam LeaveLobby: lobby=%llu", (unsigned long long)lobby);
    PFN_SteamLeaveLobby o = (PFN_SteamLeaveLobby)g_steam[11].orig;
    if (o)
        o(self, lobby);
    rules_left_lobby(lobby);
}

#ifdef NIOH3P_RESEARCH
static int EOS_CALL det_SteamSendLobbyChatMsg(void *self, uint64_t lobby, const void *data, int len)
{
    SITE_NOTE("SteamSendLobbyChatMsg");
    const uint8_t *b = (const uint8_t *)data;
    if (b && len >= 8 && len < 65536)
        LOG("Steam SendLobbyChatMsg: %d bytes (%02x %02x %02x %02x %02x %02x %02x %02x)",
            len, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    else
        LOG("Steam SendLobbyChatMsg: %d bytes", len);
    PFN_SteamSendLobbyChatMsg o = (PFN_SteamSendLobbyChatMsg)g_steam[6].orig;
    return o ? o(self, lobby, data, len) : 0;
}
#endif

#ifdef NIOH3P_RESEARCH
/* ------------------------------------------------- Steam callback plumbing ---
 * SteamAPI_RegisterCallback hands us the game's CCallback<Class, Payload> object;
 * slot 0 is Run(void *pvParam, bool ioFailure, SteamAPICall_t call). It must
 * preserve the call handle: CCallResult drops nonmatching handles. This lets us
 * log every payload the game receives (LobbyDataUpdate_t, LobbyEnter_t,
 * GameLobbyJoinRequested_t, ...) while still forwarding to the game's own Run. The
 * class names come from those vtable addresses in analysis/classmap.txt. */
#define MAX_CBRUN 24
static struct {
    void *vtbl;
    void *run;          /* the game's original Run */
    int   cb_id;
    int   hits;
} g_cbrun[MAX_CBRUN];
static int g_cbrun_n;

static void watch_callback(void *cb, int cb_id, const char *how)
{
    if (!cb || !readable(cb, sizeof(void *)))
        return;
    void **vtbl = *(void ***)cb;
    if (!readable(vtbl, 2 * sizeof(void *)))
        return;
    void *run = vtbl[0];
    {
        char cb_[240];
        /* run is read from the live slot, so on a re-registration it is already ours */
        LOG("steam cb: %s id=%d obj=%p vtbl=%p run=%p%s class %s",
            how, cb_id, cb, vtbl, run,
            run == (void *)(uintptr_t)det_CallbackRun ? " (already ours)" : "",
            site_exact((void *)vtbl, cb_, sizeof cb_));
    }
    if (!run || !readable(run, 8))
        return;
    for (int i = 0; i < g_cbrun_n; i++)
        if (g_cbrun[i].vtbl == vtbl)
            return;                                   /* already watching */
    if (g_cbrun_n >= MAX_CBRUN) {
        LOG("steam cb: watch table full");
        return;
    }
    DWORD old;
    if (!VirtualProtect(vtbl, sizeof(void *), PAGE_READWRITE, &old))
        return;
    g_cbrun[g_cbrun_n].vtbl = vtbl;
    g_cbrun[g_cbrun_n].run = run;
    g_cbrun[g_cbrun_n].cb_id = cb_id;
    g_cbrun[g_cbrun_n].hits = 0;
    vtbl[0] = (void *)(uintptr_t)det_CallbackRun;
    VirtualProtect(vtbl, sizeof(void *), old, &old);
    LOG("steam cb: Run at %p watched (cb %d)", run, cb_id);
    g_cbrun_n++;
}

static void EOS_CALL det_CallbackRun(void *self, void *param, uint8_t io_failure, uint64_t call)
{
    void *vtbl = self && readable(self, sizeof(void *)) ? *(void **)self : 0;
    for (int i = 0; i < g_cbrun_n; i++) {
        if (g_cbrun[i].vtbl != vtbl)
            continue;
        if (g_cbrun[i].hits < 40) {
            char cb_[240];
            LOG("steam cb: Run id=%d obj=%p class %s param=%p ioFailure=%u call=%llu",
                g_cbrun[i].cb_id, self, site_exact(vtbl, cb_, sizeof cb_), param,
                (unsigned)io_failure, (unsigned long long)call);
            if (param && readable(param, 16)) {
                const uint8_t *b = (const uint8_t *)param;
                LOG("steam cb: payload: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x",
                    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
            }
            g_cbrun[i].hits++;
        }
        PFN_CallbackRun o = (PFN_CallbackRun)g_cbrun[i].run;
        if (o)
            o(self, param, io_failure, call);
        return;
    }
    LOG("steam cb: Run with unknown vtable %p", vtbl);
}

static void EOS_CALL det_RegisterCallback(void *cb, int cb_id)
{
    PFN_SteamRegisterCallback o = (PFN_SteamRegisterCallback)orig_RegisterCallback;
    watch_callback(cb, cb_id, "RegisterCallback");
    if (o)
        o(cb, cb_id);
}

static void EOS_CALL det_RegisterCallResult(void *cb, uint64_t call)
{
    PFN_SteamRegisterCallResult o = (PFN_SteamRegisterCallResult)orig_RegisterCallResult;
    watch_callback(cb, -1, "RegisterCallResult");
    if (o)
        o(cb, call);
}

/* ---- ISteamMatchmaking: invites and the per-member ticket data ---- */
static int EOS_CALL det_SteamInviteUserToLobby(void *self, uint64_t lobby, uint64_t invitee)
{
    SITE_NOTE("SteamInviteUserToLobby");
    int r = g_steam[14].orig
          ? ((int EOS_CALL (*)(void *, uint64_t, uint64_t))g_steam[14].orig)(self, lobby, invitee) : 0;
    LOG("Steam InviteUserToLobby: lobby=%llu invitee=%llu -> %d",
        (unsigned long long)lobby, (unsigned long long)invitee, r);
    return r;
}

static const char *EOS_CALL det_SteamGetLobbyMemberData(void *self, uint64_t lobby, uint64_t member, const char *key)
{
    SITE_NOTE("SteamGetLobbyMemberData");
    const char *v = ((const char *EOS_CALL (*)(void *, uint64_t, uint64_t, const char *))g_steam[15].orig)
                  ? ((const char *EOS_CALL (*)(void *, uint64_t, uint64_t, const char *))g_steam[15].orig)(self, lobby, member, key) : 0;
    LOG("Steam GetLobbyMemberData: lobby=%llu member=%llu key=\"%.40s\" -> \"%.80s\"",
        (unsigned long long)lobby, (unsigned long long)member, key ? key : "", v ? v : "");
    return v;
}

static int EOS_CALL det_SteamSetLobbyMemberData(void *self, uint64_t lobby, const char *key, const char *value)
{
    SITE_NOTE("SteamSetLobbyMemberData");
    int r = ((int EOS_CALL (*)(void *, uint64_t, const char *, const char *))g_steam[16].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t, const char *, const char *))g_steam[16].orig)(self, lobby, key, value) : 0;
    LOG("Steam SetLobbyMemberData: lobby=%llu key=\"%.40s\" value=\"%.120s\" -> %d",
        (unsigned long long)lobby, key ? key : "", value ? value : "", r);
    return r;
}
#endif

/* ---- ISteamMatchmaking: the lobby-list search behind "search for a friend" ---- */
static const char *lobby_cmp_name(int c)
{
    switch (c) {
    case -2: return "equal-or-less";
    case -1: return "less";
    case  0: return "equal";
    case  1: return "greater";
    case  2: return "equal-or-greater";
    case  3: return "not-equal";
    default: return "?";
    }
}

#ifdef NIOH3P_RESEARCH
static uint64_t EOS_CALL det_SteamRequestLobbyList(void *self)
{
    uint64_t r = g_steam[17].orig ? ((uint64_t EOS_CALL (*)(void *))g_steam[17].orig)(self) : 0;
    SITE_NOTE("RequestLobbyList");
    LOG("Steam RequestLobbyList -> call %llu (results arrive as LobbyMatchList call-result)",
        (unsigned long long)r);
    return r;
}
#endif

/* Player-mode filter visibility. The search filters are the only place a
 * Secret Word can hide, and a player cannot run trace=1: the first eight
 * distinct keys that pass through the string and numerical filter detours are
 * logged by name alone (never the value), so a player log from a Secret Word
 * session lists every key the game filters on. Bounded: eight keys, no more. */
#define FILTER_KEYS 8
static char g_filter_keys[FILTER_KEYS][64];
static int  g_filter_keys_n;

static void filter_key_note(const char *key)
{
    if (g_cfg.trace || !key || !*key)
        return;                 /* trace=1 already logs each call in full */
    for (int i = 0; i < g_filter_keys_n; i++)
        if (!strcmp(g_filter_keys[i], key))
            return;
    if (g_filter_keys_n >= FILTER_KEYS)
        return;
    char *seen = g_filter_keys[g_filter_keys_n++];
    fmt(seen, 64, "%.63s", key);
    LOG("lobby filter: pass-through \"%s\"", seen);
}

static void EOS_CALL det_SteamAddStringFilter(void *self, const char *key, const char *value, int cmp)
{
    SITE_NOTE("AddRequestLobbyListStringFilter");
    if (g_cfg.drop_lock_filter && key && strstr_a(key, "lock_status")) {
        LOG("Steam lobby filter: \"%.60s\" = \"%.80s\" DROPPED (drop_lock_filter=1) - "
            "locked sessions stay visible in the search", key, value ? value : "");
        return;
    }
    filter_key_note(key);
    LOG("Steam lobby filter: \"%.60s\" %s(\"%s\") = \"%.80s\"",
        key ? key : "", lobby_cmp_name(cmp), (const char *[]){"-2","-1","0","1","2","3","?"}[cmp < -2 || cmp > 3 ? 6 : cmp + 2],
        value ? value : "");
    if (g_steam[18].orig)
        ((void EOS_CALL (*)(void *, const char *, const char *, int))g_steam[18].orig)(self, key, value, cmp);
}

static void EOS_CALL det_SteamAddNumericalFilter(void *self, const char *key, int value, int cmp)
{
    SITE_NOTE("AddRequestLobbyListNumericalFilter");
    filter_key_note(key);
    LOG("Steam lobby filter: \"%.60s\" %s %d (numeric)", key ? key : "", lobby_cmp_name(cmp), value);
    if (g_steam[19].orig)
        ((void EOS_CALL (*)(void *, const char *, int, int))g_steam[19].orig)(self, key, value, cmp);
}

#ifdef NIOH3P_RESEARCH
static void EOS_CALL det_SteamAddNearValueFilter(void *self, const char *key, int value)
{
    SITE_NOTE("AddRequestLobbyListNearValueFilter");
    LOG("Steam lobby filter: \"%.60s\" near %d", key ? key : "", value);
    if (g_steam[20].orig)
        ((void EOS_CALL (*)(void *, const char *, int))g_steam[20].orig)(self, key, value);
}
#endif

static void EOS_CALL det_SteamAddSlotsFilter(void *self, int slots)
{
    SITE_NOTE("AddRequestLobbyListFilterSlotsAvailable");
    if (g_cfg.drop_slots_filter) {
        static int noted;
        if (!noted && !g_cfg.trace) {
            noted = 1;          /* trace=1 already logs every dropped call */
            LOG("lobby filter: slots-available filter dropped (drop_slots_filter=1)");
        }
        LOG("Steam lobby filter: slots available >= %d DROPPED (drop_slots_filter=1) - "
            "full sessions stay visible in the search", slots);
        return;
    }
    LOG("Steam lobby filter: slots available >= %d", slots);
    if (g_steam[21].orig)
        ((void EOS_CALL (*)(void *, int))g_steam[21].orig)(self, slots);
}

#ifdef NIOH3P_RESEARCH
static void EOS_CALL det_SteamAddDistanceFilter(void *self, int distance)
{
    SITE_NOTE("AddRequestLobbyListDistanceFilter");
    LOG("Steam lobby filter: distance %d (0=close,1=default,2=far,3=worldwide)", distance);
    if (g_steam[22].orig)
        ((void EOS_CALL (*)(void *, int))g_steam[22].orig)(self, distance);
}

static void EOS_CALL det_SteamAddResultCountFilter(void *self, int count)
{
    SITE_NOTE("AddRequestLobbyListResultCountFilter");
    LOG("Steam lobby filter: max results %d", count);
    if (g_steam[23].orig)
        ((void EOS_CALL (*)(void *, int))g_steam[23].orig)(self, count);
}

static void EOS_CALL det_SteamAddCompatibleFilter(void *self, uint64_t lobby)
{
    SITE_NOTE("AddRequestLobbyListCompatibleMembersFilter");
    LOG("Steam lobby filter: compatible with %llu", (unsigned long long)lobby);
    if (g_steam[24].orig)
        ((void EOS_CALL (*)(void *, uint64_t))g_steam[24].orig)(self, lobby);
}

static uint64_t *EOS_CALL det_SteamGetLobbyByIndex(void *self, uint64_t *out, int index)
{
    uint64_t *r = (g_steam[25].orig && g_steam[25].slot)
                ? ((uint64_t *EOS_CALL (*)(void *, uint64_t *, int))g_steam[25].orig)(self, out, index)
                : 0;
    SITE_NOTE("GetLobbyByIndex");
    LOG("Steam GetLobbyByIndex(%d) -> lobby %llu", index, r ? (unsigned long long)*r : 0);
    return r;
}

/* ---- ISteamFriends: the session search path ---- */
static int EOS_CALL det_FriendCount(void *self, int flags)
{
    SITE_NOTE("FriendCount");
    int r = ((int EOS_CALL (*)(void *, int))g_friends[FR_COUNT].orig)
          ? ((int EOS_CALL (*)(void *, int))g_friends[FR_COUNT].orig)(self, flags) : 0;
    LOG("Steam friends: GetFriendCount(flags=0x%x) -> %d", flags, r);
    return r;
}

static uint64_t *EOS_CALL det_FriendByIndex(void *self, uint64_t *out, int index, int flags)
{
    SITE_NOTE("FriendByIndex");
    uint64_t *r = (g_friends[FR_BYINDEX].orig && g_friends[FR_BYINDEX].slot)
                ? ((uint64_t *EOS_CALL (*)(void *, uint64_t *, int, int))g_friends[FR_BYINDEX].orig)
                  (self, out, index, flags)
                : 0;
    LOG("Steam friends: GetFriendByIndex(%d, flags=0x%x) -> %llu", index, flags,
        r ? (unsigned long long)*r : 0);
    return r;
}

static int EOS_CALL det_FriendGamePlayed(void *self, uint64_t friend_id, void *info)
{
    SITE_NOTE("FriendGamePlayed");
    int r = ((int EOS_CALL (*)(void *, uint64_t, void *))g_friends[FR_GAMEPLAYED].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t, void *))g_friends[FR_GAMEPLAYED].orig)(self, friend_id, info) : 0;
    uint64_t game = 0, lobby = 0;
    if (r && readable(info, 24)) {
        game = *(uint64_t *)info;                         /* CGameID: low 32 = appid */
        lobby = *(uint64_t *)((const uint8_t *)info + 16); /* m_steamIDLobby */
    }
    LOG("Steam friends: GetFriendGamePlayed(%llu) -> %d app=%u lobby=%llu",
        (unsigned long long)friend_id, r, (unsigned)(game & 0xffffff), (unsigned long long)lobby);
    return r;
}

static int EOS_CALL det_FriendState(void *self, uint64_t friend_id)
{
    SITE_NOTE("FriendState");
    int r = ((int EOS_CALL (*)(void *, uint64_t))g_friends[FR_STATE].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t))g_friends[FR_STATE].orig)(self, friend_id) : 0;
    LOG("Steam friends: GetFriendPersonaState(%llu) -> %d", (unsigned long long)friend_id, r);
    return r;
}

static const char *EOS_CALL det_FriendRichPresence(void *self, uint64_t friend_id, const char *key)
{
    SITE_NOTE("FriendRichPresence");
    const char *v = ((const char *EOS_CALL (*)(void *, uint64_t, const char *))g_friends[FR_RICHP].orig)
                  ? ((const char *EOS_CALL (*)(void *, uint64_t, const char *))g_friends[FR_RICHP].orig)(self, friend_id, key) : 0;
    LOG("Steam friends: GetFriendRichPresence(%llu, \"%.40s\") -> \"%.160s\"",
        (unsigned long long)friend_id, key ? key : "", v ? v : "");
    return v;
}

static int EOS_CALL det_FriendRpKeyCount(void *self, uint64_t friend_id)
{
    SITE_NOTE("FriendRpKeyCount");
    int r = ((int EOS_CALL (*)(void *, uint64_t))g_friends[FR_RPCOUNT].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t))g_friends[FR_RPCOUNT].orig)(self, friend_id) : 0;
    LOG("Steam friends: GetFriendRichPresenceKeyCount(%llu) -> %d", (unsigned long long)friend_id, r);
    return r;
}

static const char *EOS_CALL det_FriendRpKeyByIndex(void *self, uint64_t friend_id, int index)
{
    SITE_NOTE("FriendRpKeyByIndex");
    const char *k = ((const char *EOS_CALL (*)(void *, uint64_t, int))g_friends[FR_RPKEY].orig)
                  ? ((const char *EOS_CALL (*)(void *, uint64_t, int))g_friends[FR_RPKEY].orig)(self, friend_id, index) : 0;
    LOG("Steam friends: GetFriendRichPresenceKeyByIndex(%llu, %d) -> \"%.40s\"",
        (unsigned long long)friend_id, index, k ? k : "");
    return k;
}

static const char *EOS_CALL det_FriendPersonaName(void *self, uint64_t friend_id)
{
    SITE_NOTE("FriendPersonaName");
    const char *n = ((const char *EOS_CALL (*)(void *, uint64_t))g_friends[FR_NAME].orig)
                  ? ((const char *EOS_CALL (*)(void *, uint64_t))g_friends[FR_NAME].orig)(self, friend_id) : 0;
    LOG("Steam friends: GetFriendPersonaName(%llu) -> \"%.60s\"", (unsigned long long)friend_id, n ? n : "");
    return n;
}

/* ---- ISteamNetworking v006: the P2P channel and its session state ---- */
/* Send and read run every frame. Logged unthrottled they wrote 336 MB in one
 * hour of normal play (1.8M lines), which no player can send anywhere. The
 * first 40 per direction and channel are logged, then every 500th, the same
 * rule the game packet trace uses. */
static volatile LONG g_p2p_count[2][8];

static int p2p_should_log(int dir, int channel)
{
    LONG n = InterlockedIncrement(&g_p2p_count[dir][channel & 7]);
    return n <= 40 || n % 500 == 0;
}

/* The game protocol rides on this channel. The first 16 payload bytes carry
 * the framing and the raw packet ID; the rest stays out of the log. */
void p2p_head(char *out, size_t cap, const void *data, unsigned size)
{
    static const char digits[] = "0123456789abcdef";
    const uint8_t *b = (const uint8_t *)data;
    size_t n = 0;
    for (unsigned i = 0; b && i < size && i < 16 && n + 3 < cap; i++) {
        out[n++] = digits[b[i] >> 4];
        out[n++] = digits[b[i] & 15];
        out[n++] = ' ';
    }
    out[n ? n - 1 : 0] = 0;
}

static int EOS_CALL det_P2PSend(void *self, uint64_t peer, const void *data, unsigned size, int send_type, int channel)
{
    int r = ((int EOS_CALL (*)(void *, uint64_t, const void *, unsigned, int, int))g_networking[P_SEND].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t, const void *, unsigned, int, int))g_networking[P_SEND].orig)(self, peer, data, size, send_type, channel) : 0;
    if (!p2p_should_log(0, channel))
        return r;
    SITE_NOTE("P2PSend");
    char head[52];
    p2p_head(head, sizeof head, data, size);
    LOG("Steam p2p: SendP2PPacket(peer=%llu size=%u type=%d chan=%d) -> %d [%s]",
        (unsigned long long)peer, size, send_type, channel, r, head);
    return r;
}

/* Polled every frame: logging it (or its call site) buried everything else
 * (1.2M lines per session), so only the read that follows a hit is logged. */
static int EOS_CALL det_P2PAvailable(void *self, unsigned *size, int channel)
{
    return ((int EOS_CALL (*)(void *, unsigned *, int))g_networking[P_AVAIL].orig)
         ? ((int EOS_CALL (*)(void *, unsigned *, int))g_networking[P_AVAIL].orig)(self, size, channel) : 0;
}

static int EOS_CALL det_P2PRead(void *self, void *dest, unsigned cap, unsigned *size, uint64_t *from, int channel)
{
    int r = ((int EOS_CALL (*)(void *, void *, unsigned, unsigned *, uint64_t *, int))g_networking[P_READ].orig)
          ? ((int EOS_CALL (*)(void *, void *, unsigned, unsigned *, uint64_t *, int))g_networking[P_READ].orig)(self, dest, cap, size, from, channel) : 0;
    if (!p2p_should_log(1, channel))
        return r;
    SITE_NOTE("P2PRead");
    char head[52];
    p2p_head(head, sizeof head, r ? dest : 0, r && size ? (*size < cap ? *size : cap) : 0);
    LOG("Steam p2p: ReadP2PPacket(chan=%d) -> %d size=%u from=%llu [%s]",
        channel, r, r && size ? *size : 0, r && from ? (unsigned long long)*from : 0ull, head);
    return r;
}

static int EOS_CALL det_P2PAccept(void *self, uint64_t peer)
{
    SITE_NOTE("P2PAccept");
    int r = ((int EOS_CALL (*)(void *, uint64_t))g_networking[P_ACCEPT].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t))g_networking[P_ACCEPT].orig)(self, peer) : 0;
    LOG("Steam p2p: AcceptP2PSessionWithUser(%llu) -> %d", (unsigned long long)peer, r);
    return r;
}

static int EOS_CALL det_P2PClose(void *self, uint64_t peer)
{
    SITE_NOTE("P2PClose");
    int r = ((int EOS_CALL (*)(void *, uint64_t))g_networking[P_CLOSE].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t))g_networking[P_CLOSE].orig)(self, peer) : 0;
    LOG("Steam p2p: CloseP2PSessionWithUser(%llu) -> %d", (unsigned long long)peer, r);
    return r;
}

static int EOS_CALL det_P2PState(void *self, uint64_t peer, void *state)
{
    SITE_NOTE("P2PState");
    int r = ((int EOS_CALL (*)(void *, uint64_t, void *))g_networking[P_STATE].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t, void *))g_networking[P_STATE].orig)(self, peer, state) : 0;
    if (r && readable(state, 20)) {
        const uint8_t *b = (const uint8_t *)state;
        LOG("Steam p2p: GetP2PSessionState(%llu) -> %d active=%u connecting=%u err=%u relay=%u queued=%d bytes",
            (unsigned long long)peer, r, b[0], b[1], b[2], b[3], *(const int *)(b + 4));
    } else {
        LOG("Steam p2p: GetP2PSessionState(%llu) -> %d", (unsigned long long)peer, r);
    }
    return r;
}

/* ---- ISteamNetworkingSockets: the newer connection path ---- */
static int EOS_CALL det_SockConnectP2P(void *self, const void *identity, int port, int n_options, const void *options)
{
    SITE_NOTE("SockConnectP2P");
    int r = ((int EOS_CALL (*)(void *, const void *, int, int, const void *))g_sockets[S_CONNECT].orig)
          ? ((int EOS_CALL (*)(void *, const void *, int, int, const void *))g_sockets[S_CONNECT].orig)(self, identity, port, n_options, options) : 0;
    int type = 0;
    uint64_t sid = 0;
    if (readable(identity, 16)) {
        type = *(const int *)identity;
        sid = *(const uint64_t *)((const uint8_t *)identity + 8);
    }
    LOG("Steam sockets: ConnectP2P(type=%d steamid=%llu port=%d) -> %d",
        type, (unsigned long long)sid, port, r);
    return r;
}

static int EOS_CALL det_SockCreateListen(void *self, int port, int n_options, const void *options)
{
    SITE_NOTE("SockCreateListen");
    int r = ((int EOS_CALL (*)(void *, int, int, const void *))g_sockets[S_LISTEN].orig)
          ? ((int EOS_CALL (*)(void *, int, int, const void *))g_sockets[S_LISTEN].orig)(self, port, n_options, options) : 0;
    LOG("Steam sockets: CreateListenSocketP2P(port=%d) -> %d", port, r);
    return r;
}

static int EOS_CALL det_SockSend(void *self, int conn, const void *data, unsigned size, int flags, int64_t *out)
{
    SITE_NOTE("SockSend");
    int r = ((int EOS_CALL (*)(void *, int, const void *, unsigned, int, int64_t *))g_sockets[S_SEND].orig)
          ? ((int EOS_CALL (*)(void *, int, const void *, unsigned, int, int64_t *))g_sockets[S_SEND].orig)(self, conn, data, size, flags, out) : 0;
    LOG("Steam sockets: SendMessageToConnection(conn=%d size=%u) -> %d", conn, size, r);
    return r;
}

static int EOS_CALL det_SockClose(void *self, int conn, int reason, const char *debug, int linger)
{
    SITE_NOTE("SockClose");
    int r = ((int EOS_CALL (*)(void *, int, int, const char *, int))g_sockets[S_CLOSE].orig)
          ? ((int EOS_CALL (*)(void *, int, int, const char *, int))g_sockets[S_CLOSE].orig)(self, conn, reason, debug, linger) : 0;
    LOG("Steam sockets: CloseConnection(conn=%d reason=%d) -> %d", conn, reason, r);
    return r;
}
#endif

static void *EOS_CALL det_FindOrCreateInterface(void *user, const char *version)
{
    void *iface = orig_FindOrCreate ? orig_FindOrCreate(user, version) : 0;
#ifdef NIOH3P_RESEARCH
    if (g_cfg.trace)
        LOG("Steam FindOrCreateUserInterface(\"%s\") -> %p", version ? version : "(null)", iface);
#endif
    if (iface && version) {
        if (!strcmp(version, "SteamMatchMaking009")) {
            steam_apply_table(iface, g_steam, NSTEAM);
#ifdef NIOH3P_RESEARCH
            if (g_cfg.trace)
                log_backtrace("Steam matchmaking interface");
        } else if (g_cfg.trace && !strcmp(version, "SteamFriends017")) {
            steam_apply_table(iface, g_friends, NFRIENDS);
            log_backtrace("Steam friends interface");
        } else if (g_cfg.trace && !strcmp(version, "SteamNetworkingSockets009")) {
            steam_apply_table(iface, g_sockets, NSOCK);
        } else if (g_cfg.trace && !strcmp(version, "SteamNetworking006")) {
            steam_apply_table(iface, g_networking, NNET);
#endif
        }
    }
    return iface;
}

#ifdef NIOH3P_RESEARCH
/* friend-invite section below: the rig's synthetic-invite trigger */
void det_SteamRunCallbacks(void);
void (*orig_RunCallbacks)(void);
#endif

void install_steam_hooks(void)
{
    if (g_steam_ready)
        return;
    HMODULE st = GetModuleHandleA("steam_api64.dll");
    if (!st)
        return;
    LOG("steam: steam_api64 at %p", (void *)st);

    /* Version strings and vtable slots are bound explicitly in the interface
     * dispatch above; unsupported Steam interfaces pass through untouched. */
    FARPROC f;
#ifdef NIOH3P_RESEARCH
    /* The synthetic-invite trigger rides the callback pump: it is the one
     * game thread the rig can rely on, and a real invite arrives through the
     * same pump. See the friend-invite section below. */
    f = GetProcAddress(st, "SteamAPI_RunCallbacks");
    if (f)
        install_hook("steam_api64.dll", "SteamAPI_RunCallbacks", (void *)f,
                     (void *)(uintptr_t)det_SteamRunCallbacks,
                     (void **)&orig_RunCallbacks);
    if (g_cfg.trace) {
        /* Callback watching and exit tracing are instrumentation only. */
        f = GetProcAddress(st, "SteamAPI_RegisterCallback");
        if (f)
            install_hook("steam_api64.dll", "SteamAPI_RegisterCallback", (void *)f,
                         (void *)(uintptr_t)det_RegisterCallback, &orig_RegisterCallback);
        f = GetProcAddress(st, "SteamAPI_RegisterCallResult");
        if (f)
            install_hook("steam_api64.dll", "SteamAPI_RegisterCallResult", (void *)f,
                         (void *)(uintptr_t)det_RegisterCallResult, &orig_RegisterCallResult);
        f = GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitProcess");
        if (f)
            install_hook("kernel32.dll", "ExitProcess", (void *)f,
                         (void *)(uintptr_t)det_ExitProcess, (void **)&orig_ExitProcess);
        f = GetProcAddress(GetModuleHandleA("kernel32.dll"), "TerminateProcess");
        if (f)
            install_hook("kernel32.dll", "TerminateProcess", (void *)f,
                         (void *)(uintptr_t)det_TerminateProcess, (void **)&orig_TerminateProcess);
    }
#endif

    /* The proxy is mapped before the game's entry point, so the interface lookup
     * that hands the game its matchmaking object always passes through here. */
    f = GetProcAddress(st, "SteamInternal_FindOrCreateUserInterface");
    if (f)
        install_hook("steam_api64.dll", "SteamInternal_FindOrCreateUserInterface",
                     (void *)f, (void *)(uintptr_t)det_FindOrCreateInterface,
                     (void **)&orig_FindOrCreate);
    else
        LOG("steam: SteamInternal_FindOrCreateUserInterface not exported");
    g_steam_ready = 1;
}

/* One SteamMatchMaking009 method by its zero-based vtable index (the same
 * numbers g_steam stores one-based). The research build may have replaced the
 * slot with a forwarding detour; g_steam[].orig is then the real method. */
void *steam_mm_method(void *self, unsigned vtable_index, int table_index)
{
    if (g_steam[table_index].orig)
        return g_steam[table_index].orig;
    if (!self || !readable(self, sizeof(void *)))
        return 0;
    void **vt = *(void ***)self;
    void **slot = vt + vtable_index;
    if (!readable(slot, sizeof(void *)) || !executable_ptr(*slot))
        return 0;
    return *slot;
}
