#include "nioh3p_internal.h"

/* ---------------------------------------------------------- lobby rules ---
 * Gameplay settings must agree on every client, so the host's file is the only
 * source once a session exists. The host publishes one lobby-data value after
 * its own data; a joining client reads it before the mission and otherwise
 * keeps the stock numbers. A visitor's nondefault file is never applied in a
 * session, so it cannot override the host.
 * Wire format, fixed order, no extras: "v1 ehp=150 adr=0 rs=1". */
#define RULES_KEY "nioh3p_rules"

Rules g_local_rules = {100, 100, 0};

/* Lobby authority is one record set behind one lock. The game thread changes
 * it from the Steam detours and the rules worker reads and refines it; a
 * native query is always performed outside the lock, and its result is
 * committed only while the generation and lobby id captured before the query
 * are still current. The lock never spans a Steam call, logging or
 * rules_apply. */
typedef struct {
    uint64_t join;          /* lobby named by the last JoinLobby request */
    uint64_t owned;         /* lobby whose rule key this process wrote */
    uint64_t published;     /* lobby id of the last accepted rule-key write */
    void *join_self;        /* matchmaking interface captured at the request */
    Rules host;             /* host value read for the join lobby */
    DWORD request_ms;       /* last RequestLobbyData, poll throttle */
    DWORD probe_ms;         /* last membership probe */
    unsigned generation;    /* bumped by every record transition */
    int create_hint;        /* a CreateLobby call handle was returned */
    int host_ok;            /* host holds an accepted host value */
    int member;             /* GetLobbyOwner(join) != 0 */
} RulesState;

static SRWLOCK g_rules_lock = SRWLOCK_INIT;
static RulesState g_rules;
/* Membership is probed once per second. GetLobbyOwner returns k_steamIDNil
 * when this process is not in the lobby, which is the only positive evidence
 * that a requested join completed. The request id is kept while that evidence
 * is missing: it grants nothing, and a later successful join can still confirm
 * and recover the host values already read for it. */
#define RULES_MEMBER_PROBE_MS 1000
static volatile LONG g_rules_last_role = -2;
static volatile LONG g_rules_bad_host_logged;
static volatile LONG g_rules_pub_fail_logged;

static int game_role(void)
{
    uint8_t *m = game_manager_ptr();
    return m ? *(const int32_t *)(m + 0x5b0) : -1;
}

static void rules_stock(Rules *r)
{
    r->enemy_hp_pct = 100;
    r->assist_drain_pct = 100;
    r->respawn_shrine = 0;
}

static void rules_local(Rules *r)
{
    *r = g_local_rules;
}

/* Short snapshot; the returned copy is the only state the caller may use. */
static void rules_snapshot(RulesState *out)
{
    AcquireSRWLockShared(&g_rules_lock);
    *out = g_rules;
    ReleaseSRWLockShared(&g_rules_lock);
}

void rules_note_create_request(void)
{
    AcquireSRWLockExclusive(&g_rules_lock);
    g_rules.create_hint = 1;
    ReleaseSRWLockExclusive(&g_rules_lock);
}

/* Record resets; the caller holds the exclusive lock and bumps generation. */
static void rules_clear_join_locked(void)
{
    Rules none;
    rules_stock(&none);
    g_rules.join = 0;
    g_rules.join_self = 0;
    g_rules.member = 0;
    g_rules.host = none;
    g_rules.host_ok = 0;
}

static void rules_clear_owned_locked(void)
{
    g_rules.owned = 0;
    g_rules.published = 0;
    g_rules.create_hint = 0;
}

/* Authority follows actual lobby membership, not the native role field and not
 * lobby metadata alone: the role field can still say host (or nothing) while a
 * join is in flight, and any process can read another lobby's data without
 * being in that lobby. The local file applies only to a lobby this process
 * owns, which only a successful rule-key write proves. A requested lobby
 * contributes the host's published value only while GetLobbyOwner reports this
 * process as a member; a pending, failed or ended join keeps the stock
 * numbers. The request id is retained while unproven because it grants nothing
 * and a slow join can still become a membership later. */
static void rules_effective(Rules *r)
{
    RulesState st;
    rules_snapshot(&st);
    if (st.owned) {
        rules_local(r);
        return;
    }
    if (!st.join || !st.member || !st.host_ok) {
        rules_stock(r);
        return;
    }
    *r = st.host;
}


void rules_apply(void)
{
    Rules r;
    rules_effective(&r);
    nioh4p_addons_rules_changed(&r);
}

static int rules_key_int(const char *tok, const char *key, int *out, int lo, int hi)
{
    size_t n = strlen(key);
    if (strncmp(tok, key, n) || tok[n] != '=')
        return 0;
    return parse_int_strict(tok + n + 1, out) && *out >= lo && *out <= hi;
}

/* Reject anything that is not exactly the four fixed tokens. */
static int rules_parse(const char *s, Rules *out)
{
    char tok[4][32];
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        int len = 0;
        while (*s && *s != ' ' && *s != '\t') {
            if (len >= (int)sizeof tok[0] - 1)
                return 0;
            if (n >= 4)
                return 0;
            tok[n][len++] = *s++;
        }
        tok[n][len] = 0;
        if (++n > 4)
            return 0;
    }
    if (n != 4 || strcmp(tok[0], "v1"))
        return 0;
    Rules r;
    if (!rules_key_int(tok[1], "ehp", &r.enemy_hp_pct, 25, 400) ||
        !rules_key_int(tok[2], "adr", &r.assist_drain_pct, 0, 1000) ||
        !rules_key_int(tok[3], "rs", &r.respawn_shrine, 0, 1))
        return 0;
    *out = r;
    return 1;
}

/* Membership evidence for the requested lobby. GetLobbyOwner returns
 * k_steamIDNil when this process is not in the lobby, so a nonzero owner means
 * the asynchronous JoinLobby completed. The virtual method writes its CSteamID
 * result through the caller-owned buffer after `this`. */
static int rules_member_of(void *self, uint64_t lobby)
{
    void *fn = steam_mm_method(self, 35, 12);  /* ISteamMatchmaking::GetLobbyOwner */
    if (!fn)
        return 0;
    uint64_t out[2] = { 0, 0 };
    typedef uint64_t *(EOS_CALL *GetOwner)(void *, uint64_t *, uint64_t);
    uint64_t *r = ((GetOwner)fn)(self, out, lobby);
    return r && *r ? 1 : 0;
}

/* Probe the retained request once per second. GetLobbyOwner is the only
 * positive membership evidence, so a nil answer keeps the id and its host
 * value but grants nothing: effectiveness stays stock until a later probe
 * confirms. There is no synthetic expiry, so a slow join that does complete
 * still applies the host value. The native query runs outside the lock and is
 * committed only for the same generation and lobby. */
static void rules_join_probe(void)
{
    RulesState st;
    rules_snapshot(&st);
    if (!st.join)
        return;
    DWORD now = GetTickCount();
    if (now - st.probe_ms < RULES_MEMBER_PROBE_MS)
        return;
    int was = st.member;
    int first = st.probe_ms == 0;
    int member = rules_member_of(st.join_self, st.join);
    int committed = 0;
    AcquireSRWLockExclusive(&g_rules_lock);
    if (g_rules.generation == st.generation && g_rules.join == st.join) {
        g_rules.member = member;
        g_rules.probe_ms = now;
        committed = 1;
    }
    ReleaseSRWLockExclusive(&g_rules_lock);
    if (!committed)
        return;
    if (member && !was)
        LOG("rules: lobby %llu membership confirmed; host rules apply",
            (unsigned long long)st.join);
    else if (!member && was)
        LOG("rules: lobby %llu membership ended; stock rules kept, request retained",
            (unsigned long long)st.join);
    else if (!member && first)
        LOG("rules: lobby %llu membership not confirmed yet; stock rules kept, request retained",
            (unsigned long long)st.join);
}

void rules_publish(void *self, uint64_t lobby)
{
    if (!lobby || !self)
        return;
    RulesState st;
    rules_snapshot(&st);
    if (!st.create_hint || st.published == lobby)
        return;
    Rules r;
    rules_local(&r);
    char value[48];
    fmt(value, (int)sizeof value, "v1 ehp=%d adr=%d rs=%d",
        r.enemy_hp_pct, r.assist_drain_pct, r.respawn_shrine);
    void *fn = steam_mm_method(self, 20, 2);   /* ISteamMatchmaking::SetLobbyData */
    if (!fn)
        return;
    typedef int (EOS_CALL *SetData)(void *, uint64_t, const char *, const char *);
    int ok = ((SetData)fn)(self, lobby, RULES_KEY, value);
    if (!ok) {
        if (InterlockedCompareExchange(&g_rules_pub_fail_logged, 1, 0) == 0)
            LOG("rules: cannot publish host rules in lobby %llu (not the owner)",
                (unsigned long long)lobby);
        return;
    }
    /* Steam accepts lobby-data writes only from the owner, so a successful
     * write is the ownership proof the CreateLobby API handle cannot give.
     * It also supersedes a pending or failed join of another lobby. */
    uint64_t dropped = 0;
    int committed = 0;
    AcquireSRWLockExclusive(&g_rules_lock);
    if (g_rules.generation == st.generation && g_rules.create_hint) {
        g_rules.owned = lobby;
        g_rules.published = lobby;
        if (g_rules.join && g_rules.join != lobby) {
            dropped = g_rules.join;
            rules_clear_join_locked();
        }
        g_rules.generation++;
        committed = 1;
    }
    ReleaseSRWLockExclusive(&g_rules_lock);
    if (!committed) {
        LOG("rules: rule-key write to lobby %llu not recorded; authority changed meanwhile",
            (unsigned long long)lobby);
        return;
    }
    LOG("rules: host rules \"%s\" published in lobby %llu (ownership confirmed)",
        value, (unsigned long long)lobby);
    if (dropped)
        LOG("rules: lobby %llu is ours; dropped pending join of lobby %llu",
            (unsigned long long)lobby, (unsigned long long)dropped);
}

/* Read the host's published rules from the lobby this process asked to join.
 * Called from the JoinLobby detour (that call is asynchronous, so membership
 * is not yet proven there) and again from the 250 ms worker until the value
 * arrives. The value is only cached; rules_effective applies it after the
 * membership probe confirms. The Steam calls run outside the rules lock. */
static void rules_poll_host(void *self, uint64_t lobby)
{
    if (!self || !lobby)
        return;
    RulesState st;
    rules_snapshot(&st);
    if (st.join != lobby)
        return;
    void *get = steam_mm_method(self, 19, 3);  /* GetLobbyData */
    if (get) {
        typedef const char *(EOS_CALL *GetData)(void *, uint64_t, const char *);
        const char *v = ((GetData)get)(self, lobby, RULES_KEY);
        if (v && *v) {
            Rules r;
            if (rules_parse(v, &r)) {
                int committed = 0;
                AcquireSRWLockExclusive(&g_rules_lock);
                if (g_rules.generation == st.generation && g_rules.join == lobby) {
                    g_rules.host = r;
                    g_rules.host_ok = 1;
                    committed = 1;
                }
                ReleaseSRWLockExclusive(&g_rules_lock);
                if (committed) {
                    LOG("rules: host value \"%s\" accepted", v);
                }
            } else if (InterlockedCompareExchange(&g_rules_bad_host_logged, 1, 0) == 0) {
                LOG("rules: host value is invalid; stock rules kept");
            }
            return;
        }
    }
    DWORD now = GetTickCount();
    int request = 0;
    AcquireSRWLockExclusive(&g_rules_lock);
    if (g_rules.generation == st.generation && g_rules.join == lobby &&
        now - g_rules.request_ms >= 2000) {
        g_rules.request_ms = now;
        request = 1;
    }
    ReleaseSRWLockExclusive(&g_rules_lock);
    if (!request)
        return;
    void *req = steam_mm_method(self, 28, 8);  /* RequestLobbyData */
    if (req) {
        typedef int (EOS_CALL *ReqData)(void *, uint64_t);
        ((ReqData)req)(self, lobby);
    }
}

void rules_join_lobby(void *self, uint64_t lobby)
{
    /* The lobby argument is the requested id; JoinLobby itself returns only an
     * API call handle (the detour records nothing without one), so this is
     * "we asked to be a client here", not proven membership. The worker probes
     * and only a positive answer lets the host value apply. */
    AcquireSRWLockExclusive(&g_rules_lock);
    if (g_rules.owned != lobby)
        rules_clear_owned_locked();  /* joining another session: ownership gone */
    rules_clear_join_locked();       /* also drops the previous host value */
    g_rules.join = lobby;
    g_rules.join_self = self;
    g_rules.probe_ms = 0;
    g_rules.generation++;
    ReleaseSRWLockExclusive(&g_rules_lock);
    LOG("rules: join requested for lobby %llu; membership not yet proven",
        (unsigned long long)lobby);
    /* Ask for the data and take it now, not up to a full worker tick later. */
    rules_poll_host(self, lobby);
}

/* Only the record that names this exact lobby is cleared. The game also calls
 * LeaveLobby for a failed join and for old ids during a transition; neither
 * may erase the lobby this process is currently using. */
void rules_left_lobby(uint64_t lobby)
{
    uint64_t joined, owned;
    int clear_join, clear_owned;
    AcquireSRWLockExclusive(&g_rules_lock);
    joined = g_rules.join;
    owned = g_rules.owned;
    clear_join = lobby && joined == lobby;
    clear_owned = lobby && owned == lobby;
    if (clear_join)
        rules_clear_join_locked();
    if (clear_owned)
        rules_clear_owned_locked();
    if (clear_join || clear_owned)
        g_rules.generation++;
    ReleaseSRWLockExclusive(&g_rules_lock);
    LOG("rules: LeaveLobby %llu: join %llu %s, owned %llu %s",
        (unsigned long long)lobby,
        (unsigned long long)joined, clear_join ? "cleared" : "kept",
        (unsigned long long)owned, clear_owned ? "cleared" : "kept");
}

static void rules_tick(void)
{
    RulesState st;
    rules_snapshot(&st);
    int role = game_role();
    if (role != g_rules_last_role) {
        g_rules_last_role = role;
        LOG("rules: role %d, host rules %s",
            role, st.host_ok ? "received" : "absent");
    }
    rules_join_probe();
    rules_snapshot(&st);
    if (st.join && !st.host_ok)
        rules_poll_host(st.join_self, st.join);
    /* Only this worker delivers notifications after startup. Native API
     * callbacks update the locked record, never an add-on's atomic cache. */
    rules_apply();
}

DWORD WINAPI rules_worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        rules_tick();
        Sleep(250);
    }
}
