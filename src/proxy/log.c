#include "nioh3p_internal.h"

/* One formatted log line; LOG() composes into a buffer of this size. */
#define LOGLINE    2048

/* ------------------------------------------------------------ file output */

static HANDLE open_append(const char *path)
{
    /* Share read only: another process must not be able to open this log for
     * writing while the game runs (a log a second process can rewrite is a
     * generic AV heuristic). Readers still open it concurrently. */
    return CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void write_handle(HANDLE h, const void *data, size_t n)
{
    DWORD wrote;
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, data, (DWORD)n, &wrote, NULL);
}

/* ---------------------------------------------------------------- logging */

static CRITICAL_SECTION g_loglock;
HANDLE g_log = INVALID_HANDLE_VALUE;
char g_dir[MAX_PATH];
char g_out[MAX_PATH];
const char *g_out_dir = g_dir;   /* where the log and the dumps go */
static int g_loglock_ready;

/* DllMain creates the lock while the loader lock is held (that is cheap and
 * needs no file I/O); worker() calls this first as well, so LOG() can never
 * enter an uninitialised critical section if the thread ever starts alone. */
void loglock_init(void)
{
    if (g_loglock_ready)
        return;
    InitializeCriticalSection(&g_loglock);
    g_loglock_ready = 1;
}

void LOG(const char *f, ...)
{
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    char line[LOGLINE];
    SYSTEMTIME st;
    va_list ap;
    EnterCriticalSection(&g_loglock);
    GetLocalTime(&st);
    int n = fmt(line, LOGLINE, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, f);
    n += vfmt(line + n, LOGLINE - n, f, ap);
    va_end(ap);
    if (n > LOGLINE - 2)
        n = LOGLINE - 2;
    line[n++] = '\n';
    write_handle(g_log, line, (size_t)n);
    LeaveCriticalSection(&g_loglock);
}

/* The log is opened exactly once per process. An existing log from an earlier
 * launch is kept as nioh3p.prev.log: players send the current log, plus the
 * previous one if the problem was there. Image dumps keep going to g_out_dir,
 * so they never follow a fallback log into %TEMP%. */
char g_log_path[MAX_PATH];   /* log file actually in use */
char g_log_note[MAX_PATH];   /* fallback explanation, empty when none */

/* Rotate the directory's log, then open it. Returns 1 and records the path. */
static int log_open_dir(const char *dir)
{
    char path[MAX_PATH], prev[MAX_PATH];
    fmt(path, MAX_PATH, "%s\\nioh3p.log", dir);
    fmt(prev, MAX_PATH, "%s\\nioh3p.prev.log", dir);
    MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);   /* no log yet: ignored */
    g_log = open_append(path);
    if (g_log == INVALID_HANDLE_VALUE)
        return 0;
    fmt(g_log_path, MAX_PATH, "%s", path);
    return 1;
}

/* out_dir when set, else the game directory; if that cannot be written, the
 * game directory, then %TEMP%. Nothing is opened twice. */
void log_init(const char *game_dir)
{
    if (log_open_dir(g_out_dir))
        return;
    if (g_out_dir != game_dir && log_open_dir(game_dir)) {
        fmt(g_log_note, MAX_PATH, "cannot write %s, using %s", g_out_dir, g_log_path);
        return;
    }
    char tmp[MAX_PATH];
    UINT n = GetTempPathA(MAX_PATH, tmp);
    if (n && n < MAX_PATH) {
        while (n > 0 && (tmp[n - 1] == '\\' || tmp[n - 1] == '/'))
            tmp[--n] = 0;
        /* GetTempPathA falls back to %USERPROFILE%, then %SYSTEMROOT%, which is
         * the Windows directory itself when the variables are missing or
         * broken. A log is never written there: it would need administrator
         * rights and no player could find it. */
        char *leaf = tmp;
        for (char *p = tmp; *p; p++)
            if (*p == '\\' || *p == '/')
                leaf = p + 1;
        if (!tmp[0] || !stricmp_a(leaf, "windows")) {
            /* No log exists, so LOG() cannot carry this: the debugger is the
             * only sink left. */
            OutputDebugStringA("nioh3p: log: no writable temp directory; log not created\n");
        } else if (log_open_dir(tmp)) {
            if (g_out_dir != game_dir)
                fmt(g_log_note, MAX_PATH, "cannot write %s or %s, using %s",
                    g_out_dir, game_dir, g_log_path);
            else
                fmt(g_log_note, MAX_PATH, "cannot write %s, using %s",
                    game_dir, g_log_path);
            return;
        }
    }
    /* No writable location: LOG() stays silent for this launch. */
}

/* --------------------------------------------------------- stack backtrace */
/* Research build only, like every other trace=1 feature. */

#ifdef NIOH3P_RESEARCH
/* Resolved lazily from ntdll; KERNEL32 alone is not enough here. */
typedef USHORT (WINAPI *PFN_CaptureStackBackTrace)(ULONG, ULONG, PVOID *, PULONG);
static PFN_CaptureStackBackTrace g_capture;

void bt_resolve(void)
{
    if (g_capture)
        return;
    HMODULE n = LoadLibraryA("ntdll.dll");
    if (n)
        g_capture = (PFN_CaptureStackBackTrace)(uintptr_t)GetProcAddress(n, "RtlCaptureStackBackTrace");
    LOG("backtrace: RtlCaptureStackBackTrace %s", g_capture ? "available" : "UNAVAILABLE");
}

/* Logs the caller chain so the game-side call site can be located in the dump. */
void log_backtrace(const char *what)
{
    char buf[640];
    int off;
    void *frames[16];
    ULONG n;
    if (!g_capture) {
        LOG("%s: caller chain unavailable", what);
        return;
    }
    n = g_capture(2, 16, frames, NULL);
    off = fmt(buf, sizeof buf, "%s: %u frame(s):", what, (unsigned)n);
    for (ULONG i = 0; i < n && off < (int)sizeof(buf) - 40; i++) {
        uint8_t *f = (uint8_t *)frames[i];
        if (g_base && f >= g_base && f < g_base + g_img_size)
            off += fmt(buf + off, (int)sizeof(buf) - off, " nioh.exe+%#x",
                       (unsigned)(f - g_base));
        else
            off += fmt(buf + off, (int)sizeof(buf) - off, " %p", f);
    }
    LOG("%s", buf);
}
#endif

/* ---- call-site symbols ---------------------------------------------------
 * Research build only. The RTTI class map (analysis/classmap.txt) gives every
 * virtual method's address, so a sidecar table lets each hook say *which* game
 * function called it. Loaded lazily from <dll dir>\nioh3p.sites.txt, one
 * "0xRVA name" per line (tools/sites.py writes it). Nothing here is required
 * for the mod to work. */
#ifdef NIOH3P_RESEARCH
#define MAX_SITES  32768
#define SITE_NAMES (2u << 20)
static struct { unsigned rva; unsigned off; } g_sites[MAX_SITES];
static char g_site_names[SITE_NAMES];
static int  g_sites_n;
static int  g_sites_done;

static unsigned s_len(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }

static void s_copy(char *d, const char *s, unsigned cap)
{
    unsigned i = 0;
    if (!d || !cap)
        return;
    while (s && s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void sites_load(void)
{
    char path[MAX_PATH];
    if (g_sites_done)
        return;
    g_sites_done = 1;
    fmt(path, MAX_PATH, "%s\\nioh3p.sites.txt", g_dir);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LOG("sites: %s not found, call sites stay as raw exe offsets", path);
        return;
    }
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > (8u << 20)) {
        CloseHandle(h);
        LOG("sites: %s size unusable", path);
        return;
    }
    char *buf = VirtualAlloc(NULL, sz + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    if (!buf || !ReadFile(h, buf, sz, &got, NULL)) {
        CloseHandle(h);
        if (buf)
            VirtualFree(buf, 0, MEM_RELEASE);
        LOG("sites: read failed");
        return;
    }
    CloseHandle(h);
    buf[got] = 0;
    unsigned names = 0;
    for (char *p = buf; *p && g_sites_n < MAX_SITES; ) {
        char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r')
            eol++;
        char keep = *eol;
        *eol = 0;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            unsigned long long v = 0;
            char *q = p + 2;
            int d;
            while ((d = hex_digit(*q)) >= 0) { v = (v << 4) | (unsigned)d; q++; }
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q && v < 0x10000000ull) {
                unsigned len = s_len(q);
                if (names + len + 1 < SITE_NAMES) {
                    g_sites[g_sites_n].rva = (unsigned)v;
                    g_sites[g_sites_n].off = names;
                    s_copy(g_site_names + names, q, len + 1);
                    names += len + 1;
                    g_sites_n++;
                }
            }
        }
        *eol = keep;
        if (!keep)
            break;
        p = eol + 1;
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    LOG("sites: %d names loaded from %s", g_sites_n, path);
}

const char *site_at(void *ret, char *buf, int cap)
{
    sites_load();
    uintptr_t const addr = (uintptr_t)ret;
    /* The Steam MatchMaking vtable hooks are entered from steam_api64.dll, not
     * from the exe: measuring those return addresses against the exe base
     * wrapped them into a meaningless exe+0x… (the sites table only knows exe
     * RVAs). Attribute the address to the module that contains it. */
    HMODULE const steam = GetModuleHandleA("steam_api64.dll");
    MEMORY_BASIC_INFORMATION mbi;
    if (steam && VirtualQuery(ret, &mbi, sizeof mbi) &&
        mbi.AllocationBase == (void *)steam) {
        fmt(buf, cap, "steam_api64+0x%x", (unsigned)(addr - (uintptr_t)steam));
        return buf;
    }
    unsigned rva = (unsigned)(addr - (uintptr_t)GetModuleHandleA(NULL));
    unsigned best = 0;
    const char *name = 0;
    for (int i = 0; i < g_sites_n; i++) {
        unsigned r = g_sites[i].rva;
        if (r <= rva && r >= best) {
            best = r;
            name = g_site_names + g_sites[i].off;
        }
    }
    if (name && rva - best < 0x8000)
        fmt(buf, cap, "%s+0x%x", name, rva - best);
    else
        fmt(buf, cap, "exe+0x%x", rva);
    return buf;
}

const char *site_exact(void *addr, char *buf, int cap)
{
    sites_load();
    unsigned rva = (unsigned)((uintptr_t)addr - (uintptr_t)GetModuleHandleA(NULL));
    for (int i = 0; i < g_sites_n; i++)
        if (g_sites[i].rva == rva) {
            fmt(buf, cap, "%s", g_site_names + g_sites[i].off);
            return buf;
        }
    fmt(buf, cap, "exe+0x%x", rva);
    return buf;
}
#endif
