/*
 * Nioh 3 Player - test harness
 *
 * A minimal Windows console program that imports dinput8.dll and
 * EOSSDK-Win64-Shipping.dll exactly the way nioh.exe does, so the dinput8 proxy
 * (nioh3p) can be exercised without launching the game:
 *   1. calls DirectInput8Create      -> proxy forwarding must work
 *   2. calls EOS_Lobby_CreateLobby   -> proxy must log and clamp MaxLobbyMembers
 *   3. calls EOS_LobbyModification_SetMaxMembers -> proxy must clamp MaxMembers
 *
 * Built freestanding (KERNEL32 only), entry point mainCRTStartup.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

typedef void (__stdcall *fn_create)(void *, const void *, void *, const void *);
typedef int32_t (__stdcall *fn_setmax)(void *, const void *);

__declspec(dllimport) HRESULT __stdcall DirectInput8Create(HINSTANCE, DWORD, const void *, void **, void *);
__declspec(dllimport) void __stdcall EOS_Lobby_CreateLobby(void *, const void *, void *, const void *);
__declspec(dllimport) int32_t __stdcall EOS_LobbyModification_SetMaxMembers(void *, const void *);
__declspec(dllimport) void __stdcall EOS_Lobby_JoinLobby(void *, const void *, void *, const void *);
__declspec(dllimport) uint32_t __stdcall EOS_LobbyDetails_GetMemberCount(void *, const void *);
__declspec(dllimport) int32_t __stdcall EOS_LobbyModification_AddAttribute(void *, const void *);
__declspec(dllimport) int32_t __stdcall EOS_LobbyDetails_CopyInfo(void *, const void *, void **);
__declspec(dllimport) int32_t __stdcall EOS_LobbySearch_SetParameter(void *, const void *);
__declspec(dllimport) void *__stdcall SteamInternal_FindOrCreateUserInterface(void *, const char *);
__declspec(dllimport) unsigned long long nioh3p_test_create_max(void);
__declspec(dllimport) uint64_t __stdcall SteamAPI_ISteamMatchmaking_CreateLobby(void *, int, int);
__declspec(dllimport) uint64_t __stdcall SteamAPI_ISteamMatchmaking_GetLobbyOwner(void *, uint64_t);
__declspec(dllimport) uint64_t __stdcall SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void *, uint64_t, int);
__declspec(dllimport) uint64_t __stdcall SteamAPI_ISteamMatchmaking_GetLobbyByIndex(void *, int);
__declspec(dllimport) uint64_t __stdcall SteamAPI_ISteamFriends_GetFriendByIndex(void *, int, int);
__declspec(dllimport) int nioh3p_test_setmax(void);
__declspec(dllimport) void *nioh3p_test_callback(void);
__declspec(dllimport) int nioh3p_test_cb_runs(void);
__declspec(dllimport) uint8_t nioh3p_test_cb_failed(void);
__declspec(dllimport) unsigned long long nioh3p_test_rp_friend(void);
__declspec(dllimport) int nioh3p_test_filter_calls(void);
__declspec(dllimport) const char *nioh3p_test_last_value(void);
__declspec(dllimport) int nioh3p_test_slots_calls(void);
__declspec(dllimport) int nioh3p_test_list_calls(void);
__declspec(dllimport) void __stdcall SteamAPI_RegisterCallResult(void *, uint64_t);

typedef struct {
    int32_t  ApiVersion;
    void    *LocalUserId;
    uint32_t MaxLobbyMembers;
    int32_t  PermissionLevel;
    int32_t  bPresenceEnabled;
    int32_t  bAllowInvites;
    const char *BucketId;
} CreateLobbyOptions;

typedef struct {
    int32_t  ApiVersion;
    uint32_t MaxMembers;
} SetMaxMembersOptions;

typedef union {
    int32_t b;
    long long i;
    double d;
    const char *s;
} AttrValue;

typedef struct {
    int32_t  ApiVersion;
    const char *Key;
    AttrValue V;
    int32_t  Type;
} AttrData;

typedef struct {
    int32_t  ApiVersion;
    const AttrData *Value;
    int32_t  Visibility;
} AttrOptions;

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

/* Freestanding substring search for the log checks (the harness has no CRT). */
static int contains(const char *h, const char *n)
{
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b)
            return 1;
    }
    return 0;
}

static void out(const char *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n = 0;
    const char *p = s;
    while (*p)
        p++;
    if (h != INVALID_HANDLE_VALUE)
        WriteFile(h, s, (DWORD)(p - s), &n, NULL);
}

/* IID_IDirectInput8W */
static const unsigned char IID_DI8W[16] = {
    0x31, 0x80, 0x79, 0xBF, 0x3A, 0x48, 0xA2, 0x4D,
    0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 };

static void num(const char *label, unsigned long v)
{
    char buf[64];
    int i = 0;
    char t[16];
    int j = 0;
    unsigned long x = v;
    out(label);
    if (!x) t[j++] = '0';
    while (x) { t[j++] = (char)('0' + x % 10); x /= 10; }
    while (j) buf[i++] = t[--j];
    buf[i++] = '\r';
    buf[i++] = '\n';
    buf[i] = 0;
    out(buf);
}

/* What the proxy should clamp to: max_players from nioh3p.ini next to this
 * exe (the same file the proxy reads). 0 turns the proxy clamps off, so the
 * raw value 2 then stays 2. A missing file or key means the proxy default 3.
 * Freestanding build, so the scan is a plain byte loop. */
static int expected_max(void)
{
    /* static: a 4 KB stack buffer would need __chkstk_ms, which the
     * freestanding link has no CRT for */
    static char buf[4096];
    char path[512];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    DWORD dir = n;
    while (dir && path[dir - 1] != '\\') dir--;
    if (!dir || dir + 13 > sizeof path)
        return 3;
    path[dir++] = 'n'; path[dir++] = 'i'; path[dir++] = 'o'; path[dir++] = 'h';
    path[dir++] = '3'; path[dir++] = 'p'; path[dir++] = '.'; path[dir++] = 'i';
    path[dir++] = 'n'; path[dir++] = 'i'; path[dir] = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 3;
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(h);
    if (!ok)
        return 3;
    buf[got] = 0;
    static const char key[] = "max_players=";
    int found = -1;   /* last occurrence wins, like the proxy parser */
    for (DWORD i = 0; i + sizeof key - 1 <= got; i++) {
        DWORD j = 0;
        while (j < sizeof key - 1 && buf[i + j] == key[j]) j++;
        if (j == sizeof key - 1) {
            int v = 0, any = 0;
            for (DWORD k = i + j; k < got && buf[k] != '\r' && buf[k] != '\n'; k++) {
                if (buf[k] < '0' || buf[k] > '9') break;
                v = v * 10 + (buf[k] - '0');
                any = 1;
            }
            if (any && v >= 0 && v <= 4)
                found = v;
        }
    }
    if (found < 0)
        return 3;
    return found ? found : 2;   /* 0: clamps off, the raw 2 stays 2 */
}

void mainCRTStartup(void)
{
    out("TEST: harness start\r\n");
    {
        HMODULE m = GetModuleHandleA("dinput8.dll");
        char path[512];
        DWORD n = 0;
        if (m) {
            n = GetModuleFileNameA(m, path, sizeof path);
            out("TEST: loaded dinput8 module = ");
            if (n)
                out(path);
            out("\r\n");
        } else {
            out("TEST: dinput8 module NOT loaded\r\n");
        }
    }

    void *di = 0;
    HRESULT hr = DirectInput8Create(GetModuleHandleA(NULL), 0x0800, IID_DI8W, &di, 0);
    out("TEST: DirectInput8Create hr = ");
    num("", (unsigned long)hr);
    (void)di;

    {
        /* diagnostics: which APIs work in this prefix */
        HANDLE h1 = CreateFileA("Z:\\tmp\\nioh3p-smoke\\game\\appendtest.txt",
                                FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        num("TEST: CreateFileA(FILE_APPEND_DATA) = ", (unsigned long)(uintptr_t)h1);
        if (h1 != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h1, "append-test\n", 12, &w, NULL);
            CloseHandle(h1);
        }
        HANDLE h2 = CreateFileA("Z:\\tmp\\nioh3p-smoke\\game\\appendtest2.txt",
                                GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        num("TEST: CreateFileA(GENERIC_WRITE) = ", (unsigned long)(uintptr_t)h2);
        if (h2 != INVALID_HANDLE_VALUE)
            CloseHandle(h2);

        SetLastError(0);
        HMODULE s1 = LoadLibraryA("C:\\windows\\system32\\dinput8.dll");
        num("TEST: LoadLibraryA(system32 dinput8) = ", (unsigned long)(uintptr_t)s1);
        num("TEST:   last error = ", (unsigned long)GetLastError());
        SetLastError(0);
        HMODULE s2 = LoadLibraryExA("dinput8.dll", NULL, 0x00000800);
        num("TEST: LoadLibraryExA(SEARCH_SYSTEM32) = ", (unsigned long)(uintptr_t)s2);
        num("TEST:   last error = ", (unsigned long)GetLastError());
    }

    Sleep(1500);   /* let the proxy install its hooks before we exercise them */

    int want = expected_max();
    num("TEST: expected clamp from nioh3p.ini = ", (unsigned long)want);

    CreateLobbyOptions o;
    o.ApiVersion = 7;   /* what the game image builds (docs/RE_EOS_CALLSITES.md §4.1) */
    o.LocalUserId = 0;
    o.MaxLobbyMembers = 2;
    o.PermissionLevel = 1;
    o.bPresenceEnabled = 0;
    o.bAllowInvites = 1;
    o.BucketId = "nioh3p-test";

    EOS_Lobby_CreateLobby(0, &o, 0, 0);
    out("\r\nTEST: MaxLobbyMembers after call = ");
    {
        char buf[8];
        int i = 0, v = (int)o.MaxLobbyMembers;
        char t[12];
        int j = 0;
        do { t[j++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (j) buf[i++] = t[--j];
        buf[i] = 0;
        out(buf);
    }
    out((int)o.MaxLobbyMembers == want ? " (clamped by proxy, expected match)\r\n"
                                       : " (NOT the expected clamp)\r\n");

    SetMaxMembersOptions m;
    m.ApiVersion = 2;
    m.MaxMembers = 2;
    EOS_LobbyModification_SetMaxMembers(0, &m);
    out((int)m.MaxMembers == want ? "TEST: SetMaxMembers clamped OK\r\n"
                                  : "TEST: SetMaxMembers NOT the expected clamp\r\n");
    if ((int)o.MaxLobbyMembers != want || (int)m.MaxMembers != want) {
        out("FAIL: EOS lobby capacity did not match the configured player count\n");
        ExitProcess(5);
    }

    {
        struct { int32_t ApiVersion; void *details; void *user; int32_t presence; } j;
        j.ApiVersion = 1; j.details = 0; j.user = 0; j.presence = 0;
        EOS_Lobby_JoinLobby(0, &j, 0, 0);
        out("TEST: JoinLobby call issued\r\n");

        struct { int32_t ApiVersion; } g;
        g.ApiVersion = 1;
        num("TEST: GetMemberCount = ", EOS_LobbyDetails_GetMemberCount(0, &g));

        AttrData ad;
        AttrOptions ao;
        ad.ApiVersion = 1; ad.Key = "NIOH3P"; ad.Type = 3;
        ad.V.s = "probe";
        ao.ApiVersion = 1; ao.Value = &ad; ao.Visibility = 1;
        EOS_LobbyModification_AddAttribute(0, &ao);
        out("TEST: AddAttribute call issued\r\n");

        struct { int32_t ApiVersion; const AttrData *Parameter; int32_t ComparisonOp; } search;
        ad.Key = "ktol_lobby_lock_status";
        ad.V.s = "ktol_lobby_lock";
        num("TEST: AddAttribute lock result = ", EOS_LobbyModification_AddAttribute(0, &ao));
        ad.V.s = "ktol_lobby_unlock";
        search.ApiVersion = 1; search.Parameter = &ad; search.ComparisonOp = 0;
        num("TEST: EOS lock search result = ", EOS_LobbySearch_SetParameter(0, &search));
        ad.Key = "unrelated_parameter";
        num("TEST: EOS unrelated search result = ", EOS_LobbySearch_SetParameter(0, &search));

        struct { int32_t ApiVersion; } ci;
        void *info = 0;
        ci.ApiVersion = 1;
        EOS_LobbyDetails_CopyInfo(0, &ci, &info);
        out("TEST: CopyInfo call issued\r\n");
    }

    {
        /* The proxy binds only the exact Nioh interface version, and clamps
         * CreateLobby through SteamMatchMaking009's documented vtable slot. */
        void *unsupported = SteamInternal_FindOrCreateUserInterface(0, "SteamMatchMaking010");
        if (!unsupported) {
            out("FAIL: unsupported Steam interface was not forwarded\n");
            ExitProcess(5);
        }
        ((uint64_t (*)(void *, int, int))(*(void ***)unsupported)[13])(unsupported, 2, 2);
        if (nioh3p_test_create_max() != 2) {
            out("FAIL: unsupported matchmaking version was patched\n");
            ExitProcess(5);
        }
        out("PASS: unsupported matchmaking version forwarded unchanged\n");
        void *iface = SteamInternal_FindOrCreateUserInterface(0, "SteamMatchMaking009");
        out("TEST: Steam interface = ");
        num("", (unsigned long)(uintptr_t)iface);
        {
            HMODULE sc = GetModuleHandleA("steamclient64.dll");
            void *(*ifn)(void) = sc ? (void *(*)(void))GetProcAddress(sc, "nioh3p_sc_iface") : 0;
            out("TEST: steamclient64 module = ");
            num("", (unsigned long)(uintptr_t)sc);
            out("TEST:   nioh3p_sc_iface = ");
            num("", (unsigned long)(uintptr_t)ifn);
            if (ifn) {
                void *o = ifn();
                out("TEST:   direct iface = ");
                num("", (unsigned long)(uintptr_t)o);
                out("TEST:   direct vtable[0] = ");
                num("", (unsigned long)(uintptr_t)(*(void ***)o));
            }
        }
        if (iface) {
            out("TEST:   vtable = ");
            num("", (unsigned long)(uintptr_t)(*(void ***)iface));
            out("TEST:   slot13 = ");
            num("", (unsigned long)(uintptr_t)((*(void ***)iface)[13]));
        }
        if (iface) {
            /* A mismatched or duplicate async result must not consume the
             * pending call; its matching error result must reach the handler. */
            void *cb = nioh3p_test_callback();
            const uint64_t pending = 0xfedcba9812345678ull;
            SteamAPI_RegisterCallResult(cb, pending);
            void **cvt = *(void ***)cb;
            void (*run)(void *, void *, uint8_t, uint64_t) =
                (void (*)(void *, void *, uint8_t, uint64_t))cvt[0];
            run(cb, cb, 0, pending + 1);
            if (nioh3p_test_cb_runs() != 0) {
                out("FAIL: unmatched call result consumed the pending call\n");
                ExitProcess(4);
            }
            run(cb, cb, 1, pending);
            run(cb, cb, 0, pending);
            if (nioh3p_test_cb_runs() != 1 || nioh3p_test_cb_failed() != 1) {
                out("FAIL: matching call result or I/O failure was lost\n");
                ExitProcess(4);
            }
            out("PASS: async result completed once with its I/O failure\n");
            /* the real layout: CreateLobby lives at vtable offset 0x68 (slot 13)
             * and SetLobbyMemberLimit at 0xf8 (slot 31) */
            void **vt = *(void ***)iface;
            uint64_t (*cf)(void *, int, int) = (uint64_t (*)(void *, int, int))vt[13];
            uint8_t (*sf)(void *, uint64_t, int) = (uint8_t (*)(void *, uint64_t, int))vt[31];
            cf(iface, 2, 2);
            if (nioh3p_test_create_max() != want ||
                !sf(iface, 0x0180000103C821A2ull, 2) || nioh3p_test_setmax() != want) {
                out("FAIL: lobby member limit did not update the requested lobby\n");
                ExitProcess(5);
            }
            out("TEST: Steam CreateLobby cMaxMembers seen by the fake service = ");
            num("", (unsigned long)nioh3p_test_create_max());
            out("TEST: Steam SetLobbyMemberLimit seen by the fake service = ");
            num("", (unsigned long)(unsigned)nioh3p_test_setmax());
            /* Native virtual calls exercise the proxy ABI without relying on a
             * flat SteamAPI thunk to discover its slot. */
            /* Flat wrappers unwrap MSVC's hidden CSteamID result buffers.
             * Test full 64-bit identities, not the truncated console display. */
            uint64_t lobby = SteamAPI_ISteamMatchmaking_GetLobbyByIndex(iface, 0);
            if (lobby != 0x0180000103C821A2ull ||
                SteamAPI_ISteamMatchmaking_GetLobbyOwner(iface, lobby) != 0x0110000103C821A2ull ||
                SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(iface, lobby, 1) != 0x0110000103C821A2ull) {
                out("FAIL: lobby or member identity was corrupted\n");
                ExitProcess(6);
            }
            out("PASS: lobby, owner and member identities preserved\n");

            /* invite + per-member data (matchmaking slots 16 and 25) */
            int (*invf)(void *, unsigned long long, unsigned long long) =
                (int (*)(void *, unsigned long long, unsigned long long))vt[16];
            int (*mdf)(void *, unsigned long long, const char *, const char *) =
                (int (*)(void *, unsigned long long, const char *, const char *))vt[25];
            out("TEST: Steam InviteUserToLobby -> ");
            num("", (unsigned long)invf(iface, 0x0180000123456789ull, 0x0110000103c821a2ull));
            out("TEST: Steam SetLobbyMemberData -> ");
            num("", (unsigned long)mdf(iface, 0x0180000123456789ull, "ktol_invite_data", "AAAAAAAA"));


            /* host path: what value does the service actually receive? */
            int (*sld)(void *, unsigned long long, const char *, const char *) =
                (int (*)(void *, unsigned long long, const char *, const char *))vt[20];
            sld(iface, 0x0180000123456789ull, "ktol_lock_status", "ktol_session_lock");
            out("TEST: SetLobbyData value seen by the fake service = \"");
            out(nioh3p_test_last_value());
            out("\"\n");

            /* the lobby-list search: filters, the request, and the result ids */
            void (*sff)(void *, const char *, const char *, int) =
                (void (*)(void *, const char *, const char *, int))vt[5];
            unsigned long long (*rlf)(void *) = (unsigned long long (*)(void *))vt[4];
            sff(iface, "ktol_lock_status", "ktol_session_unlock", 0);
            out("TEST: Steam RequestLobbyList -> ");
            num("", (unsigned long)rlf(iface));
            out("TEST: Steam GetLobbyByIndex(0) -> ");
            num("", (unsigned long)SteamAPI_ISteamMatchmaking_GetLobbyByIndex(iface, 0));
            void (*slf)(void *, int) = (void (*)(void *, int))vt[8];
            slf(iface, 1);
            out("TEST: slots filters seen by the fake service = ");
            num("", (unsigned long)nioh3p_test_slots_calls());
            out("TEST: filters/lists seen by the fake service = ");
            num("", (unsigned long)nioh3p_test_filter_calls());
            num("", (unsigned long)nioh3p_test_list_calls());

            /* Player-mode filter visibility: keys with no drop rule must be
             * forwarded unchanged and named in a `lobby filter: pass-through`
             * line. The proxy writes the log synchronously and keeps it
             * share-readable, so it can be read right after the calls. */
            void (*nnf)(void *, const char *, int, int) =
                (void (*)(void *, const char *, int, int))vt[6];
            sff(iface, "nioh3p_probe_key", "probe", 0);
            nnf(iface, "nioh3p_probe_num", 5, 0);
            {
                static char logbuf[65536];
                DWORD got = 0;
                /* The proxy opens the log with FILE_SHARE_READ, so no other
                 * process can rewrite it. This reader still opens with
                 * read+write sharing (a reader's share mode only permits other
                 * handles; the proxy's GENERIC_READ access is allowed). */
                HANDLE lh = CreateFileA("nioh3p.log", GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (lh != INVALID_HANDLE_VALUE) {
                    if (!ReadFile(lh, logbuf, sizeof logbuf - 1, &got, NULL))
                        got = 0;
                    CloseHandle(lh);
                }
                logbuf[got] = 0;
                if (!contains(logbuf, "mode: player (trace=0)")) {
                    /* trace=1 logs every filter call in full by design and has
                     * no pass-through summary: nothing to check in that mode. */
                    out("SKIP: filter pass-through lines need player mode (trace=0)\n");
                } else {
                    int str_ok = contains(logbuf,
                        "lobby filter: pass-through \"nioh3p_probe_key\"");
                    int num_ok = contains(logbuf,
                        "lobby filter: pass-through \"nioh3p_probe_num\"");
                    out(str_ok ? "PASS: unknown string filter key logged as pass-through\n"
                               : "FAIL: unknown string filter key not logged as pass-through\n");
                    out(num_ok ? "PASS: unknown numerical filter key logged as pass-through\n"
                               : "FAIL: unknown numerical filter key not logged as pass-through\n");
                    if (!str_ok || !num_ok)
                        ExitProcess(8);
                }
            }

            /* A second interface type uses its own CSteamID-returning wrapper. */
            void *fi = SteamInternal_FindOrCreateUserInterface(0, "SteamFriends017");
            if (fi) {
                void **fvt = *(void ***)fi;
                if (SteamAPI_ISteamFriends_GetFriendByIndex(fi, 1, 4) != 0x0110000103C821A2ull) {
                    out("FAIL: friend identity was corrupted\n");
                    ExitProcess(7);
                }
                out("PASS: friend identity preserved\n");
                const char *(*rp)(void *, unsigned long long, const char *) =
                    (const char *(*)(void *, unsigned long long, const char *))fvt[45];
                out("TEST: Steam GetFriendRichPresence -> \"");
                out(rp(fi, 0x0110000103c821a2ull, "connect"));
                out("\", friend seen by the game = ");
                num("", (unsigned long)nioh3p_test_rp_friend());
            }


            /* a watched read-only call: the proxy must see it through slot 17 */
            int (*mf)(void *, unsigned long long) = (int (*)(void *, unsigned long long))vt[17];
            out("TEST: Steam GetNumLobbyMembers returned = ");
            num("", (unsigned long)(unsigned)mf(iface, 0));
        }
    }

    out("TEST: lingering so the proxy can hook and dump...\r\n");
    Sleep(9000);
    out("TEST: done\r\n");
    ExitProcess(0);
}
