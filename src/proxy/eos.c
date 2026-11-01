#include "nioh3p_internal.h"

/* ------------------------------------------------------------- EOS detours */

/* ApiVersion values the tested build uses, and therefore the layouts below are
 * valid for. The shipping game builds CreateLobbyOptions with ApiVersion = 7
 * (six identical sites, docs/RE_EOS_CALLSITES.md §4.1; the sample log's api=3
 * line is the harness's value - README.md says so, and §8 of that document
 * flags the discrepancy). The game never issues SetMaxMembers, so the harness
 * (its only caller) sets the value; SetParameter carries the SDK's current
 * version. A call from another version would place every field after
 * ApiVersion at a different offset, so it is forwarded unmodified. */
#define EOS_API_CREATE_LOBBY    7
#define EOS_API_SET_MAX_MEMBERS 2
#define EOS_API_SET_PARAMETER   1

/* EOS_Lobby_CreateLobbyOptions (EOS SDK 1.x, x64 layout). ApiVersion is logged,
 * which validates the field offsets at runtime. */
typedef struct {
    int32_t  ApiVersion;         /* 0  */
    void    *LocalUserId;        /* 8  */
    uint32_t MaxLobbyMembers;    /* 16 */
    int32_t  PermissionLevel;    /* 20 */
    int32_t  bPresenceEnabled;   /* 24 */
    int32_t  bAllowInvites;      /* 28 */
    const char *BucketId;        /* 32 */
} CreateLobbyOptions;

/* EOS_LobbyModification_SetMaxMembersOptions */
typedef struct {
    int32_t  ApiVersion;
    uint32_t MaxMembers;
} SetMaxMembersOptions;

static void (EOS_CALL *orig_CreateLobby)(void *, const void *, void *, const void *);
static int32_t (EOS_CALL *orig_SetMaxMembers)(void *, const void *);

static void EOS_CALL det_CreateLobby(void *handle, const void *opts, void *client_data, const void *completion)
{
    CreateLobbyOptions *o = (CreateLobbyOptions *)opts;
    if (o && o->ApiVersion != EOS_API_CREATE_LOBBY) {
        LOG("eos: EOS_Lobby_CreateLobby ApiVersion %d != %d; forwarded unmodified",
            o->ApiVersion, EOS_API_CREATE_LOBBY);
        orig_CreateLobby(handle, opts, client_data, completion);
        return;
    }
    if (o) {
        LOG("EOS_Lobby_CreateLobby: api=%d max=%u perm=%d presence=%d invites=%d bucket=%s",
            o->ApiVersion, o->MaxLobbyMembers, o->PermissionLevel, o->bPresenceEnabled,
            o->bAllowInvites, o->BucketId ? o->BucketId : "(null)");
        if (g_cfg.max_players && o->MaxLobbyMembers < g_cfg.max_players) {
            LOG("EOS_Lobby_CreateLobby: raising MaxLobbyMembers %u -> %u",
                o->MaxLobbyMembers, g_cfg.max_players);
            o->MaxLobbyMembers = g_cfg.max_players;
        }
    } else {
        LOG("EOS_Lobby_CreateLobby: null options");
    }
#ifdef NIOH3P_RESEARCH
    if (g_cfg.trace)
        log_backtrace("EOS_Lobby_CreateLobby");
#endif
    orig_CreateLobby(handle, opts, client_data, completion);
}

static int32_t EOS_CALL det_SetMaxMembers(void *handle, const void *opts)
{
    SetMaxMembersOptions *o = (SetMaxMembersOptions *)opts;
    if (o && o->ApiVersion != EOS_API_SET_MAX_MEMBERS) {
        LOG("eos: EOS_LobbyModification_SetMaxMembers ApiVersion %d != %d; forwarded unmodified",
            o->ApiVersion, EOS_API_SET_MAX_MEMBERS);
        return orig_SetMaxMembers(handle, opts);
    }
    if (o) {
        LOG("EOS_LobbyModification_SetMaxMembers: api=%d max=%u", o->ApiVersion, o->MaxMembers);
        if (g_cfg.max_players && o->MaxMembers < g_cfg.max_players) {
            LOG("EOS_LobbyModification_SetMaxMembers: raising MaxMembers %u -> %u",
                o->MaxMembers, g_cfg.max_players);
            o->MaxMembers = g_cfg.max_players;
        }
    }
    return orig_SetMaxMembers(handle, opts);
}

/* EOS_Lobby_AttributeData: the union precedes ValueType in the SDK ABI. */
typedef struct {
    int32_t ApiVersion;
    const char *Key;
    union { int32_t AsBool; int64_t AsInt64; double AsDouble; const char *AsUtf8; } Value;
    int32_t ValueType;
} AttributeData;

static int32_t (EOS_CALL *orig_LobbySearchSetParameter)(void *, const void *);

/* EOS_LobbySearch_SetParameterOptions */
typedef struct {
    int32_t ApiVersion;
    const AttributeData *Parameter;
    int32_t ComparisonOperator;
} LobbySearchSetParameterOptions;

static int32_t EOS_CALL det_LobbySearchSetParameter(void *handle, const void *options)
{
    const LobbySearchSetParameterOptions *o = (const LobbySearchSetParameterOptions *)options;
    if (o && o->ApiVersion != EOS_API_SET_PARAMETER) {
        LOG("eos: EOS_LobbySearch_SetParameter ApiVersion %d != %d; forwarded unmodified",
            o->ApiVersion, EOS_API_SET_PARAMETER);
        return orig_LobbySearchSetParameter(handle, options);
    }
    const char *name = o && o->Parameter ? o->Parameter->Key : 0;
    if (g_cfg.drop_lock_filter && name && strstr_a(name, "lock_status")) {
        LOG("EOS_LobbySearch_SetParameter: dropping \"%.60s\" (drop_lock_filter=1)", name);
        return 0;                       /* EOS_Success, parameter simply not set */
    }
    LOG("EOS_LobbySearch_SetParameter: \"%.60s\"", name ? name : "");
    return orig_LobbySearchSetParameter(handle, options);
}

/* EOS_Lobby_JoinLobbyOptions, GetMemberCountOptions, LobbyDetails_Info and
 * CopyInfoOptions are only read by the trace-only detours below. */
#ifdef NIOH3P_RESEARCH
/* EOS_Lobby_JoinLobbyOptions */
typedef struct {
    int32_t ApiVersion;
    void   *LobbyDetailsHandle;
    void   *LocalUserId;
    int32_t bPresenceEnabled;
} JoinLobbyOptions;

/* EOS_LobbyDetails_GetMemberCountOptions */
typedef struct {
    int32_t ApiVersion;
} GetMemberCountOptions;


/* EOS_LobbyDetails_Info - field order and offsets taken from Epic's
 * eos_lobby_types.h (x64): ApiVersion, LobbyId, LobbyOwnerUserId,
 * PermissionLevel, AvailableSlots, MaxMembers, bAllowInvites. */
typedef struct {
    int32_t  ApiVersion;         /* 0  */
    const char *LobbyId;         /* 8  */
    void    *LobbyOwnerUserId;   /* 16 */
    int32_t  PermissionLevel;    /* 24 */
    uint32_t AvailableSlots;     /* 28 */
    uint32_t MaxMembers;         /* 32 */
    int32_t  bAllowInvites;      /* 36 */
} LobbyDetailsInfo;

/* EOS_LobbyDetails_CopyInfoOptions */
typedef struct {
    int32_t ApiVersion;
} CopyInfoOptions;
#endif

/* EOS_LobbyModification_AddAttributeOptions */
typedef struct {
    int32_t  ApiVersion;
    const AttributeData *Attribute;
    int32_t  Visibility;
} AddAttributeOptions;

static int32_t (EOS_CALL *orig_AddAttribute)(void *, const void *);

#ifdef NIOH3P_RESEARCH
static void (EOS_CALL *orig_JoinLobby)(void *, const void *, void *, const void *);
static uint32_t (EOS_CALL *orig_GetMemberCount)(void *, const void *);
static int32_t (EOS_CALL *orig_CopyInfo)(void *, const void *, void **);

static void EOS_CALL det_JoinLobby(void *handle, const void *opts, void *client_data, const void *completion)
{
    const JoinLobbyOptions *o = (const JoinLobbyOptions *)opts;
    if (o)
        LOG("EOS_Lobby_JoinLobby: api=%d details=%p user=%p presence=%d",
            o->ApiVersion, o->LobbyDetailsHandle, o->LocalUserId, o->bPresenceEnabled);
    if (g_cfg.trace)
        log_backtrace("EOS_Lobby_JoinLobby");
    orig_JoinLobby(handle, opts, client_data, completion);
}

static uint32_t EOS_CALL det_GetMemberCount(void *handle, const void *opts)
{
    const GetMemberCountOptions *o = (const GetMemberCountOptions *)opts;
    uint32_t count = orig_GetMemberCount(handle, opts);
    LOG("EOS_LobbyDetails_GetMemberCount: api=%d count=%u%s",
        o ? o->ApiVersion : -1, count,
        count > 2 ? "  <-- more than two members" : "");
    return count;
}

static int32_t EOS_CALL det_CopyInfo(void *handle, const void *opts, void **out_info)
{
    const CopyInfoOptions *o = (const CopyInfoOptions *)opts;
    int32_t r = orig_CopyInfo(handle, opts, out_info);
    if (r == 0 && out_info && *out_info) {
        const LobbyDetailsInfo *info = (const LobbyDetailsInfo *)*out_info;
        LOG("EOS_LobbyDetails_CopyInfo: api=%d lobby=\"%s\" perm=%d slots=%u maxMembers=%u invites=%d%s",
            o ? o->ApiVersion : -1, info->LobbyId ? info->LobbyId : "(null)",
            info->PermissionLevel, info->AvailableSlots, info->MaxMembers, info->bAllowInvites,
            info->MaxMembers > 2 ? "  <-- lobby is larger than two" : "");
        /* The game owns this result and releases it after consuming it. */
    } else {
        LOG("EOS_LobbyDetails_CopyInfo: api=%d result=%d (no info)",
            o ? o->ApiVersion : -1, (int)r);
    }
    return r;
}
#endif

/* EOS_EAttributeType */
#define EOS_AT_BOOLEAN 0
#define EOS_AT_INT64   1
#define EOS_AT_DOUBLE  2
#define EOS_AT_STRING  3

static int32_t EOS_CALL det_AddAttribute(void *handle, const void *opts)
{
    const AddAttributeOptions *o = (const AddAttributeOptions *)opts;
    if (o && o->Attribute) {
        const AttributeData *v = o->Attribute;
        const char *key = v->Key ? v->Key : "?";
        switch (v->ValueType) {
        case EOS_AT_BOOLEAN:
            LOG("EOS_LobbyModification_AddAttribute: key=\"%s\" bool=%d vis=%d",
                key, v->Value.AsBool, o->Visibility);
            break;
        case EOS_AT_INT64:
            LOG("EOS_LobbyModification_AddAttribute: key=\"%s\" int64=%lld vis=%d",
                key, (long long)v->Value.AsInt64, o->Visibility);
            break;
        case EOS_AT_DOUBLE:
            LOG("EOS_LobbyModification_AddAttribute: key=\"%s\" double=%.3f vis=%d",
                key, v->Value.AsDouble, o->Visibility);
            break;
        case EOS_AT_STRING:
            LOG("EOS_LobbyModification_AddAttribute: key=\"%s\" string=\"%s\" vis=%d",
                key, v->Value.AsUtf8 ? v->Value.AsUtf8 : "", o->Visibility);
            break;
        default:
            LOG("EOS_LobbyModification_AddAttribute: key=\"%s\" unknown type=%d vis=%d",
                key, v->ValueType, o->Visibility);
            break;
        }
    } else {
        LOG("EOS_LobbyModification_AddAttribute: api=%d (no value)",
            o ? o->ApiVersion : -1);
    }
    /* The EOS twin of ktol_lock_status: hand the SDK a copy with the value
     * replaced, never the caller's struct. */
    AddAttributeOptions copy;
    AttributeData value_copy;
    if (g_cfg.force_unlock && o && o->Attribute) {
        const AttributeData *v = o->Attribute;
        const char *key = v->Key ? v->Key : "";
        if (v->ValueType == EOS_AT_STRING && v->Value.AsUtf8 && strstr_a(key, "lock_status") &&
            strstr_a(v->Value.AsUtf8, "lock") && !strstr_a(v->Value.AsUtf8, "unlock")) {
            LOG("EOS_LobbyModification_AddAttribute: key=\"%.40s\" value \"%.40s\" -> "
                "\"ktol_lobby_unlock\" (force_unlock=1)", key, v->Value.AsUtf8);
            copy = *o;
            value_copy = *v;
            value_copy.Value.AsUtf8 = "ktol_lobby_unlock";
            copy.Attribute = &value_copy;
            return orig_AddAttribute(handle, &copy);
        }
    }
    return orig_AddAttribute(handle, opts);
}

int g_eos_hooks;     /* successfully installed EOS entry-point hooks */
int g_eos_polls;     /* install_eos_hooks calls that saw the module */

typedef struct {
    const char *name;
    void *detour;
    void **orig;
    int trace_only;     /* installed only when trace=1 */
} EosHook;

static EosHook g_eos_tbl[] = {
    { "EOS_Lobby_CreateLobby",               (void *)(uintptr_t)det_CreateLobby,    (void **)&orig_CreateLobby,    0 },
    { "EOS_LobbyModification_SetMaxMembers", (void *)(uintptr_t)det_SetMaxMembers,  (void **)&orig_SetMaxMembers,  0 },
    { "EOS_LobbySearch_SetParameter",        (void *)(uintptr_t)det_LobbySearchSetParameter,
                                                                                      (void **)&orig_LobbySearchSetParameter, 0 },
    { "EOS_LobbyModification_AddAttribute",  (void *)(uintptr_t)det_AddAttribute,   (void **)&orig_AddAttribute,   0 },
#ifdef NIOH3P_RESEARCH
    { "EOS_Lobby_JoinLobby",                 (void *)(uintptr_t)det_JoinLobby,      (void **)&orig_JoinLobby,      1 },
    { "EOS_LobbyDetails_GetMemberCount",     (void *)(uintptr_t)det_GetMemberCount, (void **)&orig_GetMemberCount, 1 },
    { "EOS_LobbyDetails_CopyInfo",           (void *)(uintptr_t)det_CopyInfo,       (void **)&orig_CopyInfo,       1 },
#endif
};
#define NEOS (int)(sizeof g_eos_tbl / sizeof g_eos_tbl[0])

/* *orig unset means the entry is not resolved yet: GetProcAddress has failed
 * so far (module still initialising), so only that entry is retried. Once a
 * hook was attempted, install_hook has published *orig and the entry is never
 * rescanned. */
static uint8_t g_eos_tried[NEOS];   /* an earlier poll already attempted this one */
static uint8_t g_eos_nocall[NEOS];  /* resolved, but this build never calls it */

/* Entries that count towards the preflight ratio: export resolved and the
 * build actually calls it. A name the game never references cannot be hooked
 * and is not a defect, so it is excluded and reported on its own line. */
/* The poll counter doubles as "the module was mapped at least once"; this
 * accessor is its named form. */
int eos_module_seen(void)
{
    return g_eos_polls != 0;
}

int eos_expected(void)
{
    int n = 0;
    for (int i = 0; i < NEOS; i++)
        if (*g_eos_tbl[i].orig && !g_eos_nocall[i] &&
            (!g_eos_tbl[i].trace_only || g_cfg.trace))
            n++;
    return n;
}

int eos_nocall_count(void)
{
    int n = 0;
    for (int i = 0; i < NEOS; i++)
        if (g_eos_nocall[i] && (!g_eos_tbl[i].trace_only || g_cfg.trace))
            n++;
    return n;
}

void eos_nocall_names(char *dst, int cap)
{
    int n = 0;
    dst[0] = 0;
    for (int i = 0; i < NEOS; i++) {
        if (!g_eos_nocall[i] || (g_eos_tbl[i].trace_only && !g_cfg.trace))
            continue;
        n += fmt(dst + n, cap - n, "%s%s", n ? ", " : "", g_eos_tbl[i].name);
        if (n >= cap - 1)
            break;
    }
}

int eos_unresolved(void)
{
    int n = 0;
    for (int i = 0; i < NEOS; i++)
        if (!*g_eos_tbl[i].orig && (!g_eos_tbl[i].trace_only || g_cfg.trace))
            n++;
    return n;
}

void eos_unresolved_names(char *dst, int cap)
{
    int n = 0;
    dst[0] = 0;
    for (int i = 0; i < NEOS; i++) {
        const char *name = g_eos_tbl[i].name;
        if (*g_eos_tbl[i].orig || (g_eos_tbl[i].trace_only && !g_cfg.trace))
            continue;
        n += fmt(dst + n, cap - n, "%s%s", n ? ", " : "", name);
        if (n >= cap - 1)
            break;
    }
}

/* EOSSDK may be mapped after this DLL (the loader resolves imports in order),
 * so the worker keeps polling while the module is absent: only entries whose
 * *orig is still unset are resolved and hooked, never the whole table again. */
void install_eos_hooks(void)
{
    HMODULE eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    if (!eos)
        return;                 /* not mapped yet: no entry to retry */
    if (!g_eos_polls++) {
        LOG("eos: EOSSDK module at %p", (void *)eos);
#ifdef NIOH3P_RESEARCH
        if (g_cfg.trace)
            bt_resolve();
#endif
    }
    for (int i = 0; i < NEOS; i++) {
        EosHook *h = &g_eos_tbl[i];
        if (*h->orig || (h->trace_only && !g_cfg.trace))
            continue;           /* hooked already, or not part of this mode */
        int retry = g_eos_tried[i];
        g_eos_tried[i] = 1;
        FARPROC target = GetProcAddress(eos, h->name);
        if (!target) {
            if (!retry)
                LOG("eos: %s is not exported", h->name);
            continue;
        }
        if (!retry)
            LOG("eos: %s resolved at %p", h->name, (void *)target);
        if (install_hook("EOSSDK-Win64-Shipping.dll", h->name, (void *)target,
                         h->detour, h->orig)) {
            g_eos_hooks++;
            if (retry)
                LOG("eos: %s hooked on poll %d", h->name, g_eos_polls);
            if (!*h->orig)
                *h->orig = (void *)target;  /* registered by name earlier */
        } else if (g_hook_fail == HOOK_FAIL_NO_CALLSITE) {
            /* The game never calls this export (EOS_LobbyModification_
             * SetMaxMembers in the tested build): not a defect, and not
             * retried, because the call site cannot appear later. */
            g_eos_nocall[i] = 1;
        }
        /* Any other failure leaves *orig set and is not rescanned either:
         * install_hook has already logged why. */
    }
}
