/*
 * nioh3p - Nioh: Complete Edition online research / 3-player-enablement mod
 *
 * Loaded as a dinput8.dll proxy: nioh.exe statically imports dinput8.dll, so the
 * Windows loader maps this DLL before the game's entry point - i.e. before the
 * VMProtect stub unpacks the encrypted .text section.
 *
 * What it does (nothing is faked):
 *   1. Forwards every real dinput8 export to the system dinput8.
 *   2. Research build only, with dump_image=1: waits for VMProtect to decrypt
 *      .text, then dumps the *runtime* module image. nioh.exe ships with an
 *      encrypted .text (entropy 8.0), so the game's code can neither be
 *      analysed nor patched statically - this dump is the prerequisite for any
 *      code-level mod.
 *   3. Hooks the EOS lobby API and raises MaxLobbyMembers / MaxMembers to the
 *      configured player count. EOSSDK-Win64-Shipping.dll and the game's import
 *      table are not encrypted, so this works without knowing the netcode.
 *      Two mechanisms: the documented IAT slot, plus a fallback that scans the
 *      main module for stored copies of the EOS entry point (a rebuilt or
 *      protected import table would defeat the first one).
 *   4. Patches the game's own two-player assumptions once .text is plaintext:
 *      the visitor allowance (shrine and Torii Gate recruitment) and, for the
 *      Torii Gate companion mode, two inline hooks (the host mirrors the
 *      preparation packets to every companion; later companions leave the
 *      shared mission-start counter to slot 1). See docs/RE_TOKOYO_COOP.md.
 *
 * Built freestanding: no CRT and no api-ms-win-crt-* imports, because Proton's
 * Wine (like plain Wine) does not ship those apisets - a CRT-linked DLL would
 * stop the game from starting. Only KERNEL32 is imported.
 *
 * Config: nioh3p.ini next to this DLL. Log: nioh3p.log in out_dir when that
 * key is set, otherwise next to this DLL. When the log cannot be written
 * there, the proxy retries next to this DLL and finally in %TEMP% (the
 * directory GetTempPathA reports, never the Windows directory), so a read-only
 * install still leaves a log.
 *
 * Two build variants. The default build is the player build that
 * tools/package.sh and install.sh ship: the trace-only instrumentation and the
 * image dumper are compiled out entirely. -DNIOH3P_RESEARCH=1 produces the
 * research build (build/research/dinput8.dll, built by the same ./build.sh).
 *
 * Within the research build, two runtime modes are selected by the `trace`
 * key in nioh3p.ini:
 *   trace=0 (player mode, the default): only the hooks that change behaviour
 *     (lobby capacity, filter drops, force_unlock, the metadata guard, the
 *     companion hooks) plus the rare lobby events (create/join/leave) and the
 *     numerical search filter (forwarded unchanged, but its key names go to
 *     the log) are installed. The
 *     per-P2P-packet, friends, socket and callback instrumentation is not
 *     installed at all - no hook, no log line - so the network path is
 *     untouched and a multi-hour session writes a small log.
 *   trace=1 (research mode): the full instrumentation of the original build:
 *     every hooked Steam/EOS call, its call-site note and a stack backtrace.
 *     The log grows by ~2,000 lines per second during a mission.
 * The log is opened once per process: an existing nioh3p.log is moved to
 * nioh3p.prev.log first, so the current and the previous launch can both be
 * sent. Image dumps always stay in out_dir (or the game directory); they never
 * follow the log to %TEMP%.
 */

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#define _CRT_USE_BUILTIN_OFFSETOF
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <dinput.h>

#ifndef EOS_CALL
#define EOS_CALL __stdcall
#endif

/* -DNIOH3P_RESEARCH=1 builds the research instrumentation (trace-only hooks,
 * backtraces, call-site names, image dumper). The player build leaves all of
 * it out of the binary; build.sh writes it to build/research/dinput8.dll. */

#define MAX_SLOTS  96
#define MAX_HOOKS  12
#define LOGLINE    2048

/* ------------------------------------------------------- freestanding libc */

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    return n == (size_t)-1 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++; y++;
    }
    return 0;
}

static int stricmp_a(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return (unsigned char)ca - (unsigned char)cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static char *strrchr_a(const char *s, int c)
{
    const char *last = 0;
    for (; *s; s++)
        if (*s == (char)c)
            last = s;
    return (char *)last;
}

static char *strstr_a(const char *h, const char *n)
{
    if (!*n)
        return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b)
            return (char *)h;
    }
    return 0;
}

static char *strcat_a(char *d, const char *s)
{
    char *p = d + strlen(d);
    while ((*p++ = *s++))
        ;
    return d;
}

static int atoi_a(const char *s)
{
    /* Wide accumulator with saturation. A wrapped value is not merely
     * unspecified: callers range-check the result (max_players accepts only 0
     * and 2..4), so "99999999999" folding into 2..4 silently changed the
     * player count. Anything that does not fit an int now saturates instead. */
    int neg = 0;
    unsigned long long v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    for (; *s >= '0' && *s <= '9'; s++)
        if (v <= 4294967295ull)
            v = v * 10 + (unsigned)(*s - '0');
    if (neg)
        return v >= 2147483648ull ? -2147483647 - 1 : -(int)v;
    return v > 2147483647ull ? 2147483647 : (int)v;
}

/* log2 without libm (entropy diagnostics only, research build) */
#ifdef NIOH3P_RESEARCH
static double log2a(double x)
{
    if (x <= 0.0)
        return 0.0;
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 1.0)  { x *= 2.0; e--; }
    double y = (x - 1.0) / (x + 1.0), y2 = y * y, t = y, s = 0.0;
    for (int i = 1; i <= 13; i += 2) {
        s += t / (double)i;
        t *= y2;
    }
    return (double)e + 2.0 * s / 0.6931471805599453;
}
#endif

/* minimal vsnprintf: %[0][width][.prec][l|ll|z](d|i|u|x|X|s|p|c|%) */
static int vfmt(char *dst, int cap, const char *f, va_list ap)
{
    int n = 0;
    char tmp[32];
    while (*f && n < cap - 1) {
        if (*f != '%') {
            dst[n++] = *f++;
            continue;
        }
        f++;
        int zero = 0, width = 0, prec = -1, alt = 0, left = 0;
        for (;; f++) {
            if (*f == '0') { zero = 1; continue; }
            if (*f == '#') { alt = 1; continue; }
            if (*f == '-') { left = 1; continue; }
            if (*f == '+' || *f == ' ') continue;
            break;
        }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int longs = 0;
        while (*f == 'l' || *f == 'z') { longs += (*f == 'l') ? 1 : 2; f++; }
        char spec = *f ? *f++ : 0;
        int len = 0;
        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (n < cap - 1 && *s && (prec < 0 || len < prec)) {
                dst[n++] = *s++;
                len++;
            }
            continue;
        } else if (spec == 'f') {
            double v = va_arg(ap, double);
            if (v < 0) { tmp[len++] = '-'; v = -v; }
            if (prec < 0) prec = 6;
            unsigned long long ip = (unsigned long long)v;
            double frac = v - (double)ip;
            char id[24];
            int idn = 0;
            do { id[idn++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
            while (idn) tmp[len++] = id[--idn];
            if (prec > 0) {
                tmp[len++] = '.';
                for (int k = 0; k < prec && len < (int)sizeof(tmp) - 2; k++) {
                    frac *= 10.0;
                    int dg = (int)frac;
                    if (dg > 9) dg = 9;
                    tmp[len++] = (char)('0' + dg);
                    frac -= (double)dg;
                }
            }
        } else if (spec == 'c') {
            tmp[len++] = (char)va_arg(ap, int);
        } else if (spec == '%') {
            tmp[len++] = '%';
        } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'p') {
            unsigned long long v;
            int neg = 0;
            if (spec == 'p') {
                v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
                spec = 'x';
            } else if (spec == 'd' || spec == 'i') {
                /* Fetch with the argument's own signed type. Read as unsigned,
                 * a negative int is already a large positive number before any
                 * sign test runs (role -1 was logged as 4294967295). Negating
                 * in unsigned arithmetic keeps INT_MIN/LLONG_MIN defined. */
                long long sv = longs >= 2 ? va_arg(ap, long long)
                             : longs == 1 ? va_arg(ap, long)
                             : va_arg(ap, int);
                neg = sv < 0;
                v = neg ? 0ull - (unsigned long long)sv : (unsigned long long)sv;
            } else if (longs >= 2) {
                v = va_arg(ap, unsigned long long);
            } else if (longs == 1) {
                v = va_arg(ap, unsigned long);
            } else {
                v = (unsigned long long)va_arg(ap, unsigned int);
            }
            char digits[24];
            int dn = 0;
            unsigned base = (spec == 'd' || spec == 'i' || spec == 'u') ? 10 : 16;
            const char *hex = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            do {
                digits[dn++] = hex[v % base];
                v /= base;
            } while (v);
            if (neg)
                digits[dn++] = '-';
            if (alt && base == 16) {
                digits[dn++] = (spec == 'X') ? 'X' : 'x';
                digits[dn++] = '0';
            }
            if (!left)
                while (width-- > dn)
                    tmp[len++] = zero ? '0' : ' ';
            while (dn)
                tmp[len++] = digits[--dn];
            if (left)
                while (width-- > dn)
                    tmp[len++] = ' ';
        } else {
            tmp[len++] = '%';
            if (spec)
                tmp[len++] = spec;
        }
        for (int i = 0; i < len && n < cap - 1; i++)
            dst[n++] = tmp[i];
    }
    /* cap <= 0 must not write at all: cap - 1 would be dst[-1], and the
     * callers that pass cap - n do reach a negative cap. */
    if (cap > 0)
        dst[n < cap ? n : cap - 1] = 0;
    return n;
}

static int fmt(char *dst, int cap, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vfmt(dst, cap, f, ap);
    va_end(ap);
    return n;
}

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
static HANDLE g_log = INVALID_HANDLE_VALUE;
static char g_dir[MAX_PATH];
static char g_out[MAX_PATH];
static const char *g_out_dir = g_dir;   /* where the log and the dumps go */
static int g_loglock_ready;

/* DllMain creates the lock while the loader lock is held (that is cheap and
 * needs no file I/O); worker() calls this first as well, so LOG() can never
 * enter an uninitialised critical section if the thread ever starts alone. */
static void loglock_init(void)
{
    if (g_loglock_ready)
        return;
    InitializeCriticalSection(&g_loglock);
    g_loglock_ready = 1;
}

static void LOG(const char *f, ...)
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
static char g_log_path[MAX_PATH];   /* log file actually in use */
static char g_log_note[MAX_PATH];   /* fallback explanation, empty when none */

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
static void log_init(const char *game_dir)
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

/* ------------------------------------------------------------------ config */

typedef struct {
    uint32_t max_players;
    int dump_image;
    int redump_after_s;
    int dump_every_s;
    int hook_eos;
    /* 0 = keep the game's own search filters (stock behaviour): the stock
     * "locked" filter still hides hosting sessions. 1 is an opt-in workaround
     * for a host that cannot be found and swallows the ktol_*lock_status
     * filter. drop_slots_filter is the same opt-in workaround for the stock
     * "free slots" filter that hides full sessions. */
    int drop_lock_filter;
    int force_unlock;       /* host: advertise the session as unlocked */
    int drop_slots_filter;
    int trace;              /* 0 = player mode, 1 = full research instrumentation */
    /* 0 = off (default): the third companion row does not exist at all, so a
     * 4-player session behaves exactly like the shipped two-row build. 1 opts
     * into the third row (element 0x73, private vtable, block C). */
    int hud_row3;
} Config;

/* Compiled-in defaults equal the shipped player config. The search filters are
 * kept by default, so an unmodified game has exactly the stock discovery rules;
 * the two drops are opt-in workarounds only. Designated init: a field reorder
 * or addition can never move a value to the wrong key. */
static Config g_cfg = {
    .max_players       = 3,
    .dump_image        = 0,
    .redump_after_s    = 0,
    .dump_every_s      = 0,
    .hook_eos          = 1,
    .drop_lock_filter  = 0,
    .force_unlock      = 0,
    .drop_slots_filter = 0,
    .trace             = 0,
    .hud_row3          = 0,
};

/* cfg_load runs before the log is open, so it only records the outcome here;
 * cfg_report writes the status line(s) once the log exists. */
enum {
    CFG_NOT_FOUND = 1,
    CFG_TOO_LARGE,
    CFG_UNREADABLE,
    CFG_LOADED,
};

static char g_cfg_path[MAX_PATH];   /* nioh3p.ini named in the status line */
static int  g_cfg_status;           /* CFG_*; never CFG_LOADED before cfg_load */
static int  g_cfg_unknown_keys;
static int  g_cfg_max_players_bad;  /* max_players was out of range */
static int  g_cfg_max_players_raw;  /* the value as parsed */

static void cfg_load(const char *dir)
{
    fmt(g_cfg_path, MAX_PATH, "%s\\nioh3p.ini", dir);
    HANDLE h = CreateFileA(g_cfg_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        g_cfg_status = CFG_NOT_FOUND;
        return;
    }
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > 1u << 20) {
        CloseHandle(h);
        g_cfg_status = CFG_TOO_LARGE;
        return;
    }
    char *buf = VirtualAlloc(NULL, sz + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    if (!buf || !ReadFile(h, buf, sz, &got, NULL)) {
        CloseHandle(h);
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        g_cfg_status = CFG_UNREADABLE;
        return;
    }
    CloseHandle(h);
    buf[got] = 0;

    char *line = buf;
    while (line && *line) {
        char *nl = line;
        while (*nl && *nl != '\n' && *nl != '\r')
            nl++;
        char save = *nl;
        *nl = 0;
        if (line[0] != '#' && line[0] != ';') {
            char *eq = line;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                *eq = 0;
                char *k = line, *v = eq + 1;
                while (*k == ' ' || *k == '\t') k++;
                char *ke = k + strlen(k);
                while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t' || ke[-1] == '\r')) *--ke = 0;
                while (*v == ' ' || *v == '\t') v++;
                int iv = atoi_a(v);
                if (!strcmp(k, "max_players"))         g_cfg.max_players = (uint32_t)iv;
                else if (!strcmp(k, "dump_image"))     g_cfg.dump_image = iv;
                else if (!strcmp(k, "redump_after_s")) g_cfg.redump_after_s = iv;
                else if (!strcmp(k, "dump_every_s"))   g_cfg.dump_every_s = iv;
                else if (!strcmp(k, "hook_eos"))       g_cfg.hook_eos = iv;
                else if (!strcmp(k, "drop_lock_filter")) g_cfg.drop_lock_filter = iv;
                else if (!strcmp(k, "force_unlock"))   g_cfg.force_unlock = iv;
                else if (!strcmp(k, "drop_slots_filter")) g_cfg.drop_slots_filter = iv;
                else if (!strcmp(k, "trace"))          g_cfg.trace = iv;
                else if (!strcmp(k, "hud_row3"))       g_cfg.hud_row3 = iv;
                else if (!strcmp(k, "out_dir")) {
                    /* Optional: keep the log and the image dumps out of the game
                     * directory (Windows-style path, e.g. Z:\home\me\nioh3p-out). */
                    fmt(g_out, (int)sizeof g_out, "%s", v);
                    int e = (int)strlen(g_out);
                    while (e > 0 && (g_out[e-1] == '\\' || g_out[e-1] == '/' || g_out[e-1] == ' '))
                        g_out[--e] = 0;
                    if (g_out[0])
                        g_out_dir = g_out;
                }
                else g_cfg_unknown_keys++;
            }
        }
        if (!save)
            break;
        line = nl + 1;
        while (*line == '\n' || *line == '\r')
            line++;
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    /* 0 turns the clamps off; 2..4 are the only member counts the patches
     * address. Four slot entries and limit-sized member arrays are confirmed
     * by a live four-client mission (docs/FINDINGS.md section 15). Anything
     * else falls back to 3. */
    if (g_cfg.max_players != 0 && (g_cfg.max_players < 2 || g_cfg.max_players > 4)) {
        g_cfg_max_players_raw = (int)g_cfg.max_players;
        g_cfg_max_players_bad = 1;
        g_cfg.max_players = 3;
    }
    g_cfg_status = CFG_LOADED;
}

/* Status line(s) for cfg_load's outcome, then the effective config. */
static void cfg_report(void)
{
    switch (g_cfg_status) {
    case CFG_NOT_FOUND:
        LOG("config: %s not found, defaults in use (max_players=%u)",
            g_cfg_path, g_cfg.max_players);
        break;
    case CFG_TOO_LARGE:
        LOG("config: %s unreadable or too large, defaults in use", g_cfg_path);
        break;
    case CFG_UNREADABLE:
        LOG("config: cannot read %s, defaults in use", g_cfg_path);
        break;
    default:
        break;
    }
    if (g_cfg_unknown_keys)
        LOG("config: %d unknown key(s) ignored", g_cfg_unknown_keys);
    if (g_cfg_max_players_bad)
        LOG("config: max_players=%d out of range; using 3", g_cfg_max_players_raw);
    LOG("config: max_players=%u dump_image=%d redump_after_s=%d hook_eos=%d out_dir=%s "
        "dump_every_s=%d drop_lock_filter=%d force_unlock=%d drop_slots_filter=%d trace=%d "
        "hud_row3=%d",
        g_cfg.max_players, g_cfg.dump_image, g_cfg.redump_after_s, g_cfg.hook_eos, g_out_dir,
        g_cfg.dump_every_s, g_cfg.drop_lock_filter, g_cfg.force_unlock, g_cfg.drop_slots_filter,
        g_cfg.trace, g_cfg.hud_row3);
}

/* ------------------------------------------------------------ PE in-memory */

static uint8_t *g_base;
static size_t g_img_size;
static int g_host_nioh;     /* host basename is nioh.exe (not the harness host) */

static IMAGE_NT_HEADERS64 *nt(void)
{
    if (!g_base)
        return NULL;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    IMAGE_NT_HEADERS64 *n = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    if (n->Signature != IMAGE_NT_SIGNATURE || n->OptionalHeader.Magic != 0x20b)
        return NULL;
    return n;
}

/* Nioh 1.24.08: internal offsets are not a Steam SDK ABI. Every game-side
 * patch and hook refuses a different executable rather than writing through
 * an unverified object layout. */
static int exe_supported(void)
{
    IMAGE_NT_HEADERS64 *image = nt();
    return image && image->FileHeader.TimeDateStamp == 0x6307a81b &&
        image->OptionalHeader.SizeOfImage == 0x306e000 &&
        image->OptionalHeader.AddressOfEntryPoint == 0x303b310 &&
        image->OptionalHeader.CheckSum == 0x19c00f0;
}

#ifdef NIOH3P_RESEARCH
/* The image dumper is research-only: it reads the 27 MB on-disk exe, checks
 * its entropy and walks the module image. The player build has no use for it. */
static IMAGE_SECTION_HEADER *find_section(const char *name)
{
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h)
        return NULL;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(h);
    for (int i = 0; i < h->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((const char *)s->Name, name, 8))
            return s;
    return NULL;
}

static double entropy(const uint8_t *p, size_t n)
{
    size_t hist[256];
    memset(hist, 0, sizeof hist);
    for (size_t i = 0; i < n; i++)
        hist[p[i]]++;
    double e = 0.0;
    for (int i = 0; i < 256; i++) {
        if (!hist[i])
            continue;
        double pr = (double)hist[i] / (double)n;
        e -= pr * log2a(pr);
    }
    return e;
}

static uint8_t *read_whole_file(const char *path, size_t *size_out)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0) {
        CloseHandle(h);
        return NULL;
    }
    uint8_t *b = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    if (!b || !ReadFile(h, b, sz, &got, NULL) || got != sz) {
        CloseHandle(h);
        if (b) VirtualFree(b, 0, MEM_RELEASE);
        return NULL;
    }
    CloseHandle(h);
    *size_out = sz;
    return b;
}

static int write_file(const char *path, const void *data, size_t n)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &wrote, NULL);
    CloseHandle(h);
    return ok && wrote == n;
}

/* -------------------------------------------------- decryption wait + dump */

/* 1 once the runtime .text no longer matches the on-disk (encrypted) copy. */
static int text_is_decrypted(uint8_t *disk, size_t disk_size)
{
    IMAGE_SECTION_HEADER *s = find_section(".text");
    if (!s)
        return 0;
    size_t n = s->Misc.VirtualSize < 0x10000 ? s->Misc.VirtualSize : 0x10000;
    uint8_t *mem = g_base + s->VirtualAddress;
    if (s->PointerToRawData + n > disk_size)
        return 0;
    size_t diff = 0;
    for (size_t i = 0; i < n; i++)
        if (mem[i] != disk[s->PointerToRawData + i])
            diff++;
    double frac = (double)diff / (double)n;
    double hm = entropy(mem, n);
    LOG("decrypt-probe: .text delta=%.1f%% runtime-entropy=%.3f", frac * 100.0, hm);
    if (hm >= 7.5)
        return 0;                       /* still (or again) encrypted */
    if (frac < 0.02)
        return 1;                       /* never encrypted: plaintext at rest */
    return frac > 0.30;                 /* unpacked at runtime */
}

static void dump_image(const char *tag)
{
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h)
        return;
    g_img_size = h->OptionalHeader.SizeOfImage;
    uint8_t *buf = VirtualAlloc(NULL, g_img_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        LOG("dump: VirtualAlloc(%zu) failed", g_img_size);
        return;
    }
    memset(buf, 0, g_img_size);
    size_t copied = 0, unreadable = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (size_t off = 0; off < g_img_size; ) {
        if (!VirtualQuery(g_base + off, &mbi, sizeof mbi))
            break;
        size_t chunk = (size_t)mbi.RegionSize;
        if (off + chunk > g_img_size)
            chunk = g_img_size - off;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) {
            memcpy(buf + off, g_base + off, chunk);
            copied += chunk;
        } else {
            unreadable += chunk;
        }
        off += chunk;
    }
    char path[MAX_PATH];
    fmt(path, MAX_PATH, "%s\\nioh3p_image_%s.bin", g_out_dir, tag);
    if (write_file(path, buf, g_img_size))
        LOG("dump: wrote %s (%u bytes, base %p, readable %u, unreadable %u)",
            path, (unsigned)g_img_size, g_base, (unsigned)copied, (unsigned)unreadable);
    else
        LOG("dump: FAILED to write %s", path);

    /* sidecar manifest so the raw image can be loaded at the right VA */
    char meta[1024];
    int n = fmt(meta, sizeof meta,
        "{\"module\":\"nioh.exe\",\"image_base\":\"0x%p\",\"image_size\":%u,"
        "\"entry_rva\":\"0x%x\",\"sections\":[",
        g_base, (unsigned)g_img_size, h->OptionalHeader.AddressOfEntryPoint);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(h);
    for (int i = 0; i < h->FileHeader.NumberOfSections && n < (int)sizeof(meta) - 128; i++, s++)
        n += fmt(meta + n, (int)sizeof(meta) - n,
            "%s{\"name\":\"%.8s\",\"va\":\"0x%x\",\"vsize\":\"0x%x\"}",
            i ? "," : "", (const char *)s->Name,
            (unsigned)s->VirtualAddress, (unsigned)s->Misc.VirtualSize);
    n += fmt(meta + n, (int)sizeof(meta) - n, "]}");
    char mpath[MAX_PATH];
    fmt(mpath, MAX_PATH, "%s\\nioh3p_image_%s.json", g_out_dir, tag);
    write_file(mpath, meta, (size_t)n);
    VirtualFree(buf, 0, MEM_RELEASE);
}
#endif

/* ---------------------------------------------------------------- hooking */

typedef struct {
    const char *name;
    void *target;               /* resolved EOS entry point we look for */
    void *detour;
    void *orig;
    int nslots;
    void **slot[MAX_SLOTS];
} Hook;

static Hook g_hooks[MAX_HOOKS];
static int g_hook_count;

static int add_slot(Hook *h, void **slot)
{
    if (h->nslots >= MAX_SLOTS) {
        LOG("hook: slot table full for %s", h->name);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        LOG("hook: VirtualProtect(%p) failed for %s (err %lu)", slot, h->name, GetLastError());
        return 0;
    }
    /* The detour reads h->orig, and the game may enter it as soon as a slot
     * holds it: publish orig before the slot points at the detour. x86 keeps
     * the store order; the barrier keeps the compiler from reordering. */
    if (!h->orig)
        h->orig = *slot;
    __asm__ volatile("" ::: "memory");
    *slot = h->detour;
    VirtualProtect(slot, sizeof(void *), old, &old);
    h->slot[h->nslots++] = slot;
    return 1;
}

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.Protect & PAGE_GUARD)
        return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
        return 0;
    return (size_t)((const uint8_t *)mbi.BaseAddress + mbi.RegionSize - (const uint8_t *)p) >= n;
}

/* A steam vtable slot must point at committed, non-guard executable memory
 * before the proxy overwrites it. */
static int executable_ptr(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.Protect & PAGE_GUARD)
        return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* The import table as the loader built it: authoritative, no scan needed. */
/* A few imports are not matched by name (the descriptor spells the symbol
 * differently, or the slot already holds a resolved address from elsewhere).
 * The slot is still the right place to intercept, so find it by value: this is
 * also the only fallback that can see a slot at all, since IAT pages are
 * read-only and the writable-pointer scan can never reach them. */
static void **find_iat_slot_by_value(void *expect)
{
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h)
        return NULL;
    DWORD imp_rva = h->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva)
        return NULL;
    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + imp_rva);
    for (; desc->Name; desc++) {
        const char *dll = (const char *)(g_base + desc->Name);
        IMAGE_THUNK_DATA64 *oft = (IMAGE_THUNK_DATA64 *)(g_base + (desc->OriginalFirstThunk
                                    ? desc->OriginalFirstThunk : desc->FirstThunk));
        IMAGE_THUNK_DATA64 *ft = (IMAGE_THUNK_DATA64 *)(g_base + desc->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if ((void *)ft->u1.Function != expect)
                continue;
            LOG("hook: slot %p in %s already holds %p", (void **)&ft->u1.Function, dll, expect);
            return (void **)&ft->u1.Function;
        }
    }
    return NULL;
}

static void **find_iat_slot(const char *dll_name, const char *symbol, void *expect, int *saw_iat)
{
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h)
        return NULL;
    DWORD imp_rva = h->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva)
        return NULL;
    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + imp_rva);
    for (; desc->Name; desc++) {
        const char *dll = (const char *)(g_base + desc->Name);
        if (stricmp_a(dll, dll_name))
            continue;
        *saw_iat = 1;
        IMAGE_THUNK_DATA64 *oft = (IMAGE_THUNK_DATA64 *)(g_base + (desc->OriginalFirstThunk
                                    ? desc->OriginalFirstThunk : desc->FirstThunk));
        IMAGE_THUNK_DATA64 *ft = (IMAGE_THUNK_DATA64 *)(g_base + desc->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                continue;
            const char *nm = (const char *)(g_base + oft->u1.AddressOfData + 2);
            if (strcmp(nm, symbol))
                continue;
            LOG("hook: import table slot for %s = %p (module exports %p)",
                symbol, (void *)ft->u1.Function, expect);
            if ((void *)ft->u1.Function == expect)
                return (void **)&ft->u1.Function;
            LOG("hook: slot holds a different address, the runtime import table was "
                "rebuild; falling back to the pointer scan");
            return NULL;
        }
    }
    return NULL;
}

/* Fallback: any writable location in the main module storing the EOS entry
 * point is a call path we can redirect. */
static int scan_and_patch_range(Hook *h, uint8_t *mod_base, size_t mod_size)
{
    int hits = 0;
    for (size_t off = 0; off < mod_size; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(mod_base + off, &mbi, sizeof mbi))
            break;
        size_t chunk = (size_t)mbi.RegionSize;
        if (off + chunk > mod_size)
            chunk = mod_size - off;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            uint8_t *b = mod_base + off;
            for (size_t i = 0; i + sizeof(void *) <= chunk; i += sizeof(void *)) {
                void **p = (void **)(b + i);
                if (*p == h->target && add_slot(h, p)) {
                    LOG("hook: pointer scan patched %p (%s)", p, h->name);
                    hits++;
                }
            }
        }
        off += chunk;
    }
    return hits;
}

/* Re-apply after the game or the protector restores a slot. */
static int repair_slots(Hook *h)
{
    int fixed = 0;
    for (int i = 0; i < h->nslots; i++) {
        if (*h->slot[i] == h->target) {
            DWORD old;
            if (VirtualProtect(h->slot[i], sizeof(void *), PAGE_READWRITE, &old)) {
                *h->slot[i] = h->detour;
                VirtualProtect(h->slot[i], sizeof(void *), old, &old);
                fixed++;
            }
        }
    }
    return fixed;
}

/* Why install_hook last returned 0. The EOS installer needs to tell "this
 * build never calls the export" (known, harmless) from a real failure. */
enum { HOOK_FAIL_NONE = 0, HOOK_FAIL_TABLE_FULL, HOOK_FAIL_NO_CALLSITE };
static int g_hook_fail;

static int install_hook(const char *dll_name, const char *name, void *target,
                        void *detour, void **orig_out)
{
    g_hook_fail = HOOK_FAIL_NONE;
    for (int i = 0; i < g_hook_count; i++)      /* installer runs repeatedly */
        if (!strcmp(g_hooks[i].name, name))
            return 1;
    if (g_hook_count >= MAX_HOOKS) {
        g_hook_fail = HOOK_FAIL_TABLE_FULL;
        LOG("hook: table full, cannot hook %s", name);
        return 0;
    }
    Hook *h = &g_hooks[g_hook_count];
    h->name = name;
    h->target = target;
    h->detour = detour;
    h->orig = target;
    h->nslots = 0;
    *orig_out = target;

    int saw_iat = 0;
    void **slot = find_iat_slot(dll_name, name, target, &saw_iat);
    if (!slot)
        slot = find_iat_slot_by_value(target);      /* by-value fallback */
    if (slot && add_slot(h, slot))
        LOG("hook: %s hooked through the import table (%p -> %p)", name, slot, detour);
    else
        LOG("hook: %s not usable in the import table (present=%d), scanning writable memory",
            name, saw_iat);
    if (h->nslots == 0) {
        int hits = scan_and_patch_range(h, g_base, g_img_size);
        LOG("hook: pointer scan patched %d location(s) for %s", hits, name);
    }
    if (h->nslots == 0) {
        g_hook_fail = HOOK_FAIL_NO_CALLSITE;
        LOG("hook: %s NOT hooked (no call site found in this build)", name);
        return 0;
    }
    g_hook_count++;
    return 1;
}

/* --------------------------------------------------------- stack backtrace */
/* Research build only, like every other trace=1 feature. */

#ifdef NIOH3P_RESEARCH
/* Resolved lazily from ntdll; KERNEL32 alone is not enough here. */
typedef USHORT (WINAPI *PFN_CaptureStackBackTrace)(ULONG, ULONG, PVOID *, PULONG);
static PFN_CaptureStackBackTrace g_capture;

static void bt_resolve(void)
{
    if (g_capture)
        return;
    HMODULE n = LoadLibraryA("ntdll.dll");
    if (n)
        g_capture = (PFN_CaptureStackBackTrace)(uintptr_t)GetProcAddress(n, "RtlCaptureStackBackTrace");
    LOG("backtrace: RtlCaptureStackBackTrace %s", g_capture ? "available" : "UNAVAILABLE");
}

/* Logs the caller chain so the game-side call site can be located in the dump. */
static void log_backtrace(const char *what)
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

/* ------------------------------------------------------- Steam matchmaking */


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

static const char *site_at(void *ret, char *buf, int cap)
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

static const char *site_exact(void *addr, char *buf, int cap)
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

/* Player mode does not resolve call sites at all: no sites_load(), no line.
 * The player build does not even carry the tables. */
#ifdef NIOH3P_RESEARCH
#define SITE_NOTE(label) do { \
    if (g_cfg.trace) { \
        char sb_[240]; \
        LOG("site: %s from %s", (label), site_at(__builtin_return_address(0), sb_, sizeof sb_)); \
    } \
} while (0)
#else
#define SITE_NOTE(label) ((void)0)
#endif

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
typedef struct {
    const char *name;
    void *det;
    unsigned slot_index;
    int trace_only;             /* installed only when trace=1 */
    void *orig;
    void **slot;
} SteamHook;
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
static SteamHook g_steam[] = {
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
static int steam_method_count(void)
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

static int g_steam_ready;

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

static int steam_repair(void)
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

static void install_game_metadata_guard(void);

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
    return o ? o(self, lobby, key, use) : 0;
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
static void p2p_head(char *out, size_t cap, const void *data, unsigned size)
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
static void det_SteamRunCallbacks(void);
static void (*orig_RunCallbacks)(void);
#endif

static void install_steam_hooks(void)
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

static int g_eos_hooks;     /* successfully installed EOS entry-point hooks */
static int g_eos_polls;     /* install_eos_hooks calls that saw the module */

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
static int eos_expected(void)
{
    int n = 0;
    for (int i = 0; i < NEOS; i++)
        if (*g_eos_tbl[i].orig && !g_eos_nocall[i] &&
            (!g_eos_tbl[i].trace_only || g_cfg.trace))
            n++;
    return n;
}

static int eos_nocall_count(void)
{
    int n = 0;
    for (int i = 0; i < NEOS; i++)
        if (g_eos_nocall[i] && (!g_eos_tbl[i].trace_only || g_cfg.trace))
            n++;
    return n;
}

static void eos_nocall_names(char *dst, int cap)
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

static int eos_unresolved(void)
{
    int n = 0;
    for (int i = 0; i < NEOS; i++)
        if (!*g_eos_tbl[i].orig && (!g_eos_tbl[i].trace_only || g_cfg.trace))
            n++;
    return n;
}

static void eos_unresolved_names(char *dst, int cap)
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
static void install_eos_hooks(void)
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

/* ------------------------------------------------- game-side host capacity */

static void *EOS_CALL game_create_param_set(void *, void *, const void *);
static Hook g_game_create_param_hook = {
    .name = "CSessionP2pOpenCreateParam::SetParam",
    .detour = (void *)(uintptr_t)game_create_param_set,
};

static void *EOS_CALL game_create_param_set(void *self, void *out, const void *options)
{
    typedef void *(EOS_CALL *SetParam)(void *, void *, const void *);
    void *result = ((SetParam)g_game_create_param_hook.orig)(self, out, options);
    unsigned *count = (unsigned *)((uint8_t *)self + 0x20);
    if (*count < g_cfg.max_players) {
        LOG("game create request: raising max members %u -> %u before allocation",
            *count, g_cfg.max_players);
        *count = g_cfg.max_players;
    }
    return result;
}

static void install_game_capacity_hook(void)
{
    if (!exe_supported()) {
        LOG("game capacity hook: unsupported executable; internal hook not installed");
        return;
    }
    void **vtable = (void **)(g_base + 0x1586d80);
    if (!readable(vtable, 4 * sizeof(void *)) ||
        vtable[0] != g_base + 0x5f0e00 || vtable[1] != g_base + 0x5f0ec0 ||
        vtable[2] != g_base + 0x5f0ff0 || vtable[3] != g_base + 0x5f11c0) {
        LOG("game capacity hook: create-parameter vtable mismatch; not installed");
        return;
    }
    g_game_create_param_hook.orig = vtable[2];
    if (add_slot(&g_game_create_param_hook, &vtable[2]))
        LOG("game capacity hook: create request is clamped before native member allocation");
}

/* The Steam metadata publisher can receive its own attribute descriptors.
 * After publishing, its replacement path frees the descriptor's buffer,
 * overwrites that same descriptor with a new buffer, then copies the new
 * uninitialized buffer onto itself. Publication still needs to run; only
 * the redundant local replacement is skipped for exact self-assignment. */
static void *volatile g_game_metadata_resume __attribute__((used));
static void *volatile g_game_metadata_next __attribute__((used));

__attribute__((naked)) static void game_metadata_assign_guard(void)
{
    __asm__ volatile(
        "movq %rbx, %rdi\n"
        "shlq $5, %rdi\n"
        "addq %r15, %rdi\n"
        "movq 8(%r14), %rax\n"
        "addq %rsi, %rax\n"
        "leaq 0xd0(%rdi), %r10\n"
        "cmpq %r10, %rax\n"
        "je 1f\n"
        "cmpq $0, 0xe8(%rdi)\n"
        "jmp *g_game_metadata_resume(%rip)\n"
        "1: jmp *g_game_metadata_next(%rip)\n"
    );
}

static void install_game_metadata_guard(void)
{
    typedef struct { uint64_t lo, hi; } Pair16;
    static int attempted;
    if (attempted || !g_game_create_param_hook.nslots)
        return;
    attempted = 1;
    /* Install from the publisher's API call, once this code page is active.
     * Replay the overwritten instructions on the non-alias path, including
     * the comparison flags consumed by the original branch at 0x5ffea2.
     * The guard writes RAX and R10, which the original bytes did not; that is
     * safe here: after resuming at 0x5ffea2 both paths write RAX before any
     * read (je -> 0x5ffeea mov 0x8(%r14),%rax; fall-through 0x5ffea4
     * mov 0x0(%r13),%eax), R10 is not read before the next call (0x5ffedc /
     * 0x5fff3f / 0x5fff5e), and the alias target 0x5fff63 directly follows a
     * call, so RAX/R10 are dead there. RDI/RBX/RSI/R14/R15 are only written or
     * read exactly as the original does, and RSP is untouched (no call). */
    static const uint8_t expected[] = {
        0x48, 0x8b, 0xfb, 0x48, 0xc1, 0xe7, 0x05, 0x49, 0x03, 0xff,
        0x48, 0x83, 0xbf, 0xe8, 0, 0, 0, 0,
    };
    uint8_t *site = g_base + 0x5ffe90;
    /* cmpxchg16b requires a 16-byte aligned destination. */
    if ((uintptr_t)site & 15) {
        LOG("game metadata guard: publisher site is not 16-byte aligned; not installed");
        return;
    }
    if (!readable(site, sizeof expected) || memcmp(site, expected, sizeof expected)) {
        LOG("game metadata guard: publisher instructions mismatch; not installed");
        return;
    }
    void *target = (void *)(uintptr_t)game_metadata_assign_guard;
    /* One lock cmpxchg16b publishes bytes 0..15: the 6-byte jmp [rip+0], the
     * guard address and the current bytes at site+14..15. Bytes 16..17 are
     * unreachable after the jump and stay untouched. A memcpy compiles to
     * several stores, and another thread entering the site mid-copy would
     * execute a torn instruction. */
    uint8_t patch[16];
    patch[0] = 0xff; patch[1] = 0x25;
    patch[2] = patch[3] = patch[4] = patch[5] = 0;
    memcpy(patch + 6, &target, sizeof target);
    memcpy(patch + 14, site + 14, 2);
    Pair16 want, have;
    memcpy(&want.lo, expected, 8);
    memcpy(&want.hi, expected + 8, 8);
    memcpy(&have.lo, patch, 8);
    memcpy(&have.hi, patch + 8, 8);
    DWORD old;
    /* Keep the page executable for the whole write window: other game threads
     * may be running code anywhere in this 4 KB page, and a non-executable
     * page faults (DEP) there. The page is RWX only for the cmpxchg16b and the
     * flush, then explicitly RX again. */
    if (!VirtualProtect(site, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) {
        LOG("game metadata guard: cannot make publisher writable (err %lu)", GetLastError());
        return;
    }
    g_game_metadata_resume = g_base + 0x5ffea2;
    g_game_metadata_next = g_base + 0x5fff63;
    unsigned char ok;
    __asm__ volatile(
        "lock cmpxchg16b %1\n\t"
        "sete %0"
        : "=q"(ok), "+m"(*(Pair16 *)site)
        : "a"(want.lo), "d"(want.hi), "b"(have.lo), "c"(have.hi)
        : "cc", "memory");
    if (ok)
        FlushInstructionCache(GetCurrentProcess(), site, sizeof patch);
    if (!VirtualProtect(site, sizeof patch, PAGE_EXECUTE_READ, &old))
        LOG("game metadata guard: cannot restore execute protection (err %lu)", GetLastError());
    if (!ok) {
        LOG("game metadata guard: publisher bytes changed during install; not installed");
        return;
    }
    LOG("game metadata guard: exact self-assignment keeps the existing attribute buffer");
}

/* Host visitor allowance. The session root (0x141790a70) keeps the number of
 * ordinary visitors the host may admit at +0x39c. The admission handler for
 * raw packet 16 rejects a request (raw packet 7, reason 2) unless
 * allowance - admitted ordinary visitors > 0, so the stock value 1 refuses a
 * second visitor even with three native member slots. Every store that opens
 * recruitment with that single-visitor constant gets max_players - 1 instead;
 * consumption, departure and close paths are unchanged. Code is plaintext only
 * after the stub has decrypted .text, so wait for the exact original bytes.
 * Sites install individually: the mission and shrine paths decrypt during
 * ordinary play, the torii-side store and the second setter call only after
 * that menu is used, and one dormant site must not block the others.
 * The setter 0x140d5fb30 rejects arguments >= 3 (cmp $3, jge at 0x140d5fb4a);
 * when three visitors are configured its immediate is raised too, otherwise
 * the setter keeps the stock cap and stays at 2. */
typedef struct {
    uint32_t rva;           /* first byte of the instruction */
    uint8_t len;
    uint8_t imm;            /* offset of the immediate's low byte */
    uint8_t setter;         /* value is an argument to 0x140d5fb30 */
    const char *name;
    uint8_t bytes[10];
} AllowanceSite;

static const AllowanceSite g_allowance_sites[] = {
    /* session root reset: movl $1, 0x39c(%rcx) */
    {0xd7ac0c, 10, 6, 0, "session root reset",
     {0xc7, 0x81, 0x9c, 0x03, 0, 0, 0x01, 0, 0, 0}},
    /* shrine offering opens recruitment: movb $1, 0x39c(%rbx) */
    {0xd7bb77, 7, 6, 0, "shrine offering",
     {0xc6, 0x83, 0x9c, 0x03, 0, 0, 0x01}},
    /* controller reset: movb $1, R+0x39c */
    {0xd7ca03, 7, 6, 0, "controller reset",
     {0xc6, 0x05, 0x02, 0x44, 0xa1, 0x00, 0x01}},
    /* start recruiting: movb $1, R+0x39c */
    {0xd7daa8, 7, 6, 0, "start recruiting",
     {0xc6, 0x05, 0x5d, 0x33, 0xa1, 0x00, 0x01}},
    /* second start-recruiting store (torii-side flow): movb $1, R+0x39c */
    {0xd7de1f, 7, 6, 0, "start recruiting (alt)",
     {0xc6, 0x05, 0xe6, 0x2f, 0xa1, 0x00, 0x01}},
    /* online shrine offering: mov $1, %ecx; call 0x140d5fb30 */
    {0xcf9f36, 10, 1, 1, "shrine offering setter",
     {0xb9, 0x01, 0, 0, 0, 0xe8, 0xf0, 0x5b, 0x06, 0x00}},
    /* second offering (torii-side flow): mov $2, %ecx; call 0x140d5fb30 */
    {0xcfa515, 10, 1, 1, "torii offering setter",
     {0xb9, 0x02, 0, 0, 0, 0xe8, 0x11, 0x56, 0x06, 0x00}},
};

/* setter cap: cmp $3, %ebx; jge skip at 0x140d5fb4a */
static const AllowanceSite g_setter_cap_site = {
    0xd5fb4a, 4, 2, 2, "setter cap", {0x83, 0xfb, 0x03, 0x7d},
};

static void install_game_visitor_allowance(void)
{
    if (g_cfg.max_players < 3 || !g_game_create_param_hook.nslots)
        return;
    uint8_t allowance = (uint8_t)(g_cfg.max_players > 4 ? 3 : g_cfg.max_players - 1);
    const unsigned n = sizeof g_allowance_sites / sizeof g_allowance_sites[0];
    /* A new allowance site must fail the build, not overflow this bitmap. */
    _Static_assert(sizeof g_allowance_sites / sizeof g_allowance_sites[0] <= 8,
                   "installed[] must cover every allowance site");
    uint8_t installed[8] = {0};
    unsigned done = 0;
    int cap_done = 0;
    /* VMProtect decrypts .text lazily: poll tightly for the first minute (the
     * mission start is the interesting window), then slowly for nine more. */
    for (int waited = 0; waited < 600000 && done < n; ) {
        for (unsigned i = 0; i < n; i++) {
            const AllowanceSite *s;
            uint8_t *imm, value;
            DWORD old;
            if (installed[i])
                continue;
            s = &g_allowance_sites[i];
            if (!readable(g_base + s->rva, s->len) ||
                memcmp(g_base + s->rva, s->bytes, s->len))
                continue;
            /* With three visitors configured, hold setter-argument sites
             * until the cap is raised: the stock cap rejects anything >= 3,
             * and a patched site stores once, so installing it first would
             * silently keep the stock 2 for the process lifetime. */
            if (s->setter && allowance > 2 && !cap_done)
                continue;
            /* The page must stay executable while the immediate is stored:
             * another game thread can execute anywhere in this 4 KB page, and
             * a non-executable mapping would fault (DEP). RWX only for the
             * store and the flush, then RX again. */
            imm = g_base + s->rva + s->imm;
            value = allowance;
            if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
                LOG("game visitor allowance: cannot write %#x (err %lu)", s->rva, GetLastError());
                continue;
            }
            *imm = value;
            FlushInstructionCache(GetCurrentProcess(), imm, 1);
            if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READ, &old))
                LOG("game visitor allowance: cannot restore execute protection at %#x (err %lu)",
                    s->rva, GetLastError());
            installed[i] = 1;
            done++;
            LOG("game visitor allowance: %s opens %u visitor slot(s)",
                s->name, value);
        }
        /* Raise the setter's rejection threshold to allowance + 1 before the
         * setter call sites carry more than the stock 2. */
        if (!cap_done && allowance > 2 &&
            readable(g_base + g_setter_cap_site.rva, g_setter_cap_site.len) &&
            !memcmp(g_base + g_setter_cap_site.rva, g_setter_cap_site.bytes,
                    g_setter_cap_site.len)) {
            uint8_t *imm = g_base + g_setter_cap_site.rva + g_setter_cap_site.imm;
            DWORD old;
            if (VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
                *imm = (uint8_t)(allowance + 1);
                FlushInstructionCache(GetCurrentProcess(), imm, 1);
                if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READ, &old))
                    LOG("game visitor allowance: cannot restore execute protection at %#x (err %lu)",
                        g_setter_cap_site.rva, GetLastError());
                cap_done = 1;
                LOG("game visitor allowance: setter cap raised to %u", allowance + 1);
            } else {
                LOG("game visitor allowance: cannot write setter cap %#x (err %lu)",
                    g_setter_cap_site.rva, GetLastError());
            }
        }
        if (done < n) {
            int step = waited < 60000 ? 10 : 250;
            Sleep((DWORD)step);
            waited += step;
        }
    }
    LOG("game visitor allowance: recruitment opens %u visitor slot(s); %u/%u site(s) patched%s",
        allowance, done, n, cap_done ? "; setter cap raised" : "");
}

/* --------------------------------- Torii Gate companion (Tokoyo Coop) for 3-4 */

/* Yokai Realm with a Companion is written for two players. Two places keep a
 * second companion (member slot 2 or 3) out of the mission:
 *
 * 1. Preparation. The host sends the mission preview (packet 76), its ready
 *    flag (74), the start flag (75) and the chosen mission (17) to slot 1
 *    only, so a later companion waits on "Waiting for mission to be chosen".
 *    The send hook mirrors these four packets to slots 2 and 3. Packet 17
 *    carries the receiver's own slot at payload byte 6 (the visitor's handler
 *    adopts it), so each copy gets its own slot. The send byte-swaps its
 *    buffer in place, so the copies come from a snapshot taken before.
 * 2. Mission start. Online::AddFlagWaitCoopPlayer (0x1408af1b0) is the
 *    mission script's sync point: the host bumps mission flag 0x46, a visitor
 *    bumps 0x47, and Online::IsWaitCoopPlayer holds each side until the other
 *    catches up. 0x47 is one shared counter, so with two companions it runs
 *    ahead of the host (host 1, visitors 3) and every client waits forever on
 *    the loading screen. Only the slot-1 companion advances it; later
 *    companions follow the host and slot 1.
 *
 * Game protocol entry points (docs/RE_TOKOYO_COOP.md):
 *   0x140d6a560 send(id, data, len <= 0x100, target): target 0-3 is one
 *               member slot, 4 or more is every connected slot but our own.
 *   0x140d6acc0 dispatch(record, sender), research trace only: record+0x10
 *               is the u16 raw id, record+0x18 the network-order payload.
 * The hooks replace whole prologue instructions with a 14-byte absolute jump.
 * They install at boot, before any session exists, so no game thread runs
 * these functions while the bytes change. */
#define GAME_ROOT_RVA      0x1790a70u  /* session root; +0x3c8 = own slot index */
#define GAME_COOP_MODE_RVA 0x191de34u  /* 1 while a Tokoyo companion session prepares */
#define GAME_MANAGER_RVA   0x18715e0u  /* online manager; +0x5b0 role, +0x660 Tokoyo */

typedef uint8_t (*PFN_GameSend)(unsigned, const uint8_t *, unsigned, int);
typedef void (*PFN_WaitFlag)(void);
static PFN_GameSend g_game_send;
static PFN_WaitFlag g_wait_flag;

static int tokoyo_fanout_wanted(unsigned id, int target)
{
    if (target != 1 || g_cfg.max_players < 3)
        return 0;
    if (id != 17 && id != 74 && id != 75 && id != 76)
        return 0;
    const uint8_t *own = g_base + GAME_ROOT_RVA + 0x3c8;
    const int32_t *mode = (const int32_t *)(g_base + GAME_COOP_MODE_RVA);
    return readable(own, 1) && readable(mode, 4) && *own == 0 && *mode == 1;
}

#ifdef NIOH3P_RESEARCH
typedef uintptr_t (*PFN_GameDispatch)(uint8_t *, int);
static PFN_GameDispatch g_game_dispatch;
static volatile LONG g_pkt_count[2][128];

/* The admission decision reads root+0x39c (visitors still allowed) and rejects
 * with raw packet 7, reason 2, when the counter reaches zero. The departure
 * path at 0x140d7b234 decrements that same counter and clamps it at zero, so a
 * host cannot readmit a companion until the whole session ends. Log the counter
 * and the slot table whenever an admission request (16) or a reject (7)
 * crosses, which is the evidence the rejoin fix needs. */
static void log_session_state(const char *what, unsigned id, int peer)
{
    const uint8_t *root = g_base + GAME_ROOT_RVA;
    if (!readable(root, 0x4ec)) {
        LOG("session: %s id=%u (root unreadable)", what, id);
        return;
    }
    char slots[32];
    int n = 0;
    for (int s = 0; s < 4; s++) {
        const uint8_t *rec = root + (size_t)s * 0xa8;
        if (!readable(rec, 0x10a))
            break;
        n += fmt(slots + n, (int)sizeof slots - n, "%s%u", s ? "," : "", rec[0xe8]);
    }
    slots[n < (int)sizeof slots ? n : (int)sizeof slots - 1] = 0;
    LOG("session: %s id=%u peer=%d allow=%u mode=%d own=%u sel=%d prof=%u slot_e8=[%s]",
        what, id, peer, root[0x39c], *(const int32_t *)(g_base + GAME_COOP_MODE_RVA),
        root[0x3c8], *(const int32_t *)(root + 0x4e8), root[0xcc], slots);
}

static int pkt_should_log(int dir, unsigned id)
{
    LONG n = InterlockedIncrement(&g_pkt_count[dir][id & 127]);
    return n <= 40 || n % 500 == 0;
}

/* Raw 77 carries one companion's 128-byte profile and the game keeps only the
 * last one in root+0x4c, so the second companion erases the first. Keep one
 * profile per sender slot here; this is the data a cycling panel needs. */
static uint8_t g_profile[4][0x80];
static unsigned g_profile_seen;

static void keep_profile(int slot, const uint8_t *payload)
{
    if (slot < 0 || slot >= 4)
        return;
    memcpy(g_profile[slot], payload, 0x80);
    g_profile_seen |= 1u << slot;
    char head[52];
    p2p_head(head, sizeof head, payload, 16);
    LOG("game tokoyo: profile slot %d stored (seen=%#x) [%s]", slot, g_profile_seen, head);
}

static uintptr_t det_GameDispatch(uint8_t *record, int sender)
{
    unsigned id = readable(record, 0x28) ? *(uint16_t *)(record + 0x10) : 0xffff;
    if (g_cfg.trace && id == 16)          /* admission request */
        log_session_state("recv", id, sender);
    if (g_cfg.trace && id == 77 && readable(record + 0x18, 0x80))
        keep_profile(sender, record + 0x18);
    if (g_cfg.trace && pkt_should_log(1, id)) {
        char head[52];
        p2p_head(head, sizeof head, record ? record + 0x18 : 0, id != 0xffff ? 16 : 0);
        LOG("game pkt recv: id=%u from=%d #%ld [%s]", id, sender, g_pkt_count[1][id & 127], head);
    }
    return g_game_dispatch(record, sender);
}
#endif

static uint8_t det_GameSend(unsigned id, const uint8_t *data, unsigned len, int target)
{
    id &= 0xffff;
    uint8_t pristine[0x100];
    int fan = tokoyo_fanout_wanted(id, target) && len <= sizeof pristine && readable(data, len);
    if (fan)
        memcpy(pristine, data, len);
    uint8_t r = g_game_send(id, data, len, target);
#ifdef NIOH3P_RESEARCH
    if (g_cfg.trace && id == 7)           /* admission reject, reason in byte 2 */
        log_session_state("send", id, target);
    if (g_cfg.trace && pkt_should_log(0, id)) {
        char head[52];
        p2p_head(head, sizeof head, data, readable(data, len < 16 ? len : 16) ? len : 0);
        LOG("game pkt send: id=%u len=%u target=%d -> %u #%ld [%s]", id, len, target, r,
            g_pkt_count[0][id & 127], head);
    }
#endif
    /* Slots are 0-based: the host is 0, so max_players=N owns slots 0..N-1. */
    const int last_slot = (int)g_cfg.max_players;
    for (int slot = 2; fan && slot < last_slot; slot++) {
        uint8_t copy[0x100];
        memcpy(copy, pristine, len);
        if (id == 17 && len > 6)
            copy[6] = (uint8_t)slot;
        if (g_game_send(id, copy, len, slot))
            LOG("game tokoyo: packet %u also sent to companion slot %d", id, slot);
    }
    return r;
}

static void det_WaitFlag(void)
{
    uint8_t *const *manager = (uint8_t *const *)(g_base + GAME_MANAGER_RVA);
    const uint8_t *own = g_base + GAME_ROOT_RVA + 0x3c8;
    if (g_cfg.max_players >= 3 && readable(manager, 8) && *manager &&
        readable(*manager + 0x5b0, 0xb4) && readable(own, 1) &&
        *(*manager + 0x660) && *(const int32_t *)(*manager + 0x5b0) == 1 && *own >= 2) {
        LOG("game tokoyo: companion slot %u leaves the shared wait counter to slot 1", *own);
        return;
    }
    g_wait_flag();
}

typedef struct {
    uint32_t rva;
    uint8_t n;              /* whole instructions replaced at the site */
    const char *name;
    void *detour;
    void **orig;
    uint8_t bytes[24];
    /* NULL: the n site bytes are position independent and run as copied.
     * Otherwise this writes an equivalent of them and returns its length. */
    unsigned (*relocate)(uint8_t *out);
} InlineHook;

/* sub $0x28,%rsp; mov 0x18715e0(%rip),%rax; mov 0x5b0(%rax),%ecx, with the
 * RIP-relative load turned into movabs from the absolute address. */
static unsigned relocate_wait_flag(uint8_t *out)
{
    uintptr_t manager = (uintptr_t)(g_base + GAME_MANAGER_RVA);
    static const uint8_t head[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0xa1};
    static const uint8_t tail[] = {0x8b, 0x88, 0xb0, 0x05, 0x00, 0x00};
    memcpy(out, head, sizeof head);
    memcpy(out + sizeof head, &manager, 8);
    memcpy(out + sizeof head + 8, tail, sizeof tail);
    return sizeof head + 8 + sizeof tail;
}

/* ------------------------------------------- party HUD: the second row -- */

/* The mission HUD builds two MultiPlayerGauge rows, element ids 0x74 and 0x75.
 * Only the first is drawn. Both carry a live copy of a companion's HP, and the
 * row shows that companion's own name, so the hidden row is a complete second
 * companion row. Its state word at +0x28 holds 0x01000000 while hidden and
 * 0x01010001 when drawn. Setting the two missing bits makes the row appear and
 * it stays up. Limits: the HUD has two rows in total, so a fourth player still
 * has none, and this only runs when max_players >= 3.
 * Evidence: docs/RE_TOKOYO_COOP.md, "Party HUD". */
#define HUD_ROW_CTOR_RVA   0xc1e230u
#define HUD_ROW_ID_OFF     0x8u
#define HUD_ROW_FLAGS_OFF  0x28u
#define HUD_ROW_SHOWN      0x01010001u
#define HUD_ROWS_MAX       4

static void *(*g_hud_row_ctor)(void *self, int id, int index);
static void *g_hud_rows[HUD_ROWS_MAX];
static volatile LONG g_hud_row_count;
static volatile LONG g_hud_row_worker_started;

/* 48 89 4c 24 08   mov %rcx,0x8(%rsp)
 * 53               push %rbx
 * 48 83 ec 30      sub $0x30,%rsp
 * 48 c7 44 24 20 fe ff ff ff   movq $-2,0x20(%rsp)
 * The trampoline runs one return address deeper, so both stack displacements
 * move by 8. */
static unsigned relocate_hud_row(uint8_t *out)
{
    static const uint8_t code[] = {
        0x48, 0x89, 0x4c, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec, 0x30,
        0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff,
    };
    memcpy(out, code, sizeof code);
    return sizeof code;
}

static void hud_row_show(void *row)
{
    uint8_t *const flags = (uint8_t *)row + HUD_ROW_FLAGS_OFF;
    if (!readable(flags, 4))
        return;
    const uint32_t now = *(const uint32_t *)flags;
    if ((now & HUD_ROW_SHOWN) == HUD_ROW_SHOWN)
        return;
    *(volatile uint32_t *)flags = now | HUD_ROW_SHOWN;
}

/* Only a companion session gets the second row; another mode that happens to
 * build this gauge class keeps its own layout. The byte is logged whenever it
 * changes: the worker's silence after a tick is otherwise indistinguishable
 * from "mode off". */
#define GAME_LOADER_OFF        0x5d0u   /* manager +0x5d0: mission loader phase */
#define GAME_LOADER_IN_MISSION 0xcu     /* 0xC = in the mission (docs/RE_TOKOYO_COOP.md) */

static volatile LONG g_hud_row_tokoyo_last = -1;

static int hud_row_tokoyo(void)
{
    uint8_t *const *manager = (uint8_t *const *)(g_base + GAME_MANAGER_RVA);
    int on = -1;
    if (readable(manager, 8) && *manager && readable(*manager + 0x660, 1))
        on = *(*manager + 0x660) ? 1 : 0;
    if (g_cfg.hud_row3 && on != (int)InterlockedExchange(&g_hud_row_tokoyo_last, on))
        LOG("game hud: tokoyo mode %s (byte %d, manager %p)",
            on < 0 ? "unreadable" : on ? "on" : "off", on,
            readable(manager, 8) ? (void *)*manager : NULL);
    return on == 1;
}

static int hud_row_wanted(void)
{
    return g_cfg.max_players >= 3 && hud_row_tokoyo();
}

/* ------------------------------------------- party HUD: the third row -- */

/* A fourth player still has no row: the HUD builds exactly two
 * MultiPlayerGauge elements (ids 0x74, 0x75) and the class hard-codes two
 * state blocks, A (0x1b46a90) and B (0x1b46b08), selected by
 * "element id - 0x74 == 0 ? A : (== 1 ? B : A)" in 15 places. Neither the
 * element count nor that selection is data, so this section does not patch
 * the game: it creates a third element with id 0x73 (whose id maps to block
 * A), gives it a *private vtable* whose block-touching slots run against a
 * proxy-owned block C by swapping A <-> C around each call, and then feeds C
 * a companion character the two native rows do not use.
 *
 * Facts this relies on, all from the runtime image dump (see
 * docs/RE_TOKOYO_COOP.md, "Party HUD"):
 *   - class vtable 0x12729c0, 158 slots (0x4f0 bytes), first non-code qword
 *     after it is 0x200000001; slots 0x00/0x08/0x10/0x18/0x50/0xb8/0xd8 hold
 *     the only methods of the class that touch A or B; slot 0xa8 is the id
 *     getter and must stay original.
 *   - element registry: [0x189f400] is a manager whose first qword array is
 *     indexed by element id (id < 0x137, one element per id, NULL = free);
 *     0xbfacc0 registers, 0x140dd1cb3 gates sprites through it.
 *   - row creation as the live builder 0x140b3c220 does it: menu from
 *     0xfa6fd0, factory menu->vt[0x28](size, &{0x2d,param}), ctor 0xc1e230,
 *     init through vt[0x08], vt[0x70] marks it ready, then register.
 *   - placement: the container sprite at element+0x60 is moved by reading a
 *     layout anchor (0xc58930(elem, vec, group)) and writing group 0
 *     (0xdda790(sprite, 0, vec)); the native rows use groups 0x5f and 0x60,
 *     so the third row is row2 + (row2 - row1).
 *   - companion characters live in the static party slot array at 0x18a0490,
 *     stride 0x18, field +0 = character (0x755dc0 is the game's accessor);
 *     block+0x30 is the character a row renders, block+0x00 its state (0 =
 *     empty, 2 = populated in every mission dump).
 *   - the empty state block is 0x78 bytes and 0xc1eb10 resets it; the ctor
 *     calls that reset for whichever block its id selects. */
#define HUD_ROW3_ID          0x73
#define HUD_ROW3_SETTLE      2          /* worker ticks with both rows present */
#define HUD_ROW3_VTABLE_RVA  0x12729c0u
#define HUD_ROW3_VTABLE_SIZE 0x4f0u     /* measured extent, not the slot count */
#define HUD_ROW3_BLOCK_A_RVA 0x1b46a90u
#define HUD_ROW3_BLOCK_B_RVA 0x1b46b08u
#define HUD_ROW3_BLOCK_SIZE  0x78u
#define HUD_ROW3_STATE_OFF   0x00u      /* 0 empty, 2 populated */
#define HUD_ROW3_CHAR_OFF    0x30u      /* companion character object */
#define HUD_ROW3_SPRITES_OFF 0x48u      /* four sprite pointers, 0x78-byte block */
#define HUD_ROW3_CONTAINER_OFF 0x60u    /* element+0x60: the row's container sprite */
#define HUD_ROW3_STATE_READY 2
#define HUD_ROW3_ELEMS_RVA   0x189f400u /* [.] = element registry base */
#define HUD_ROW3_MENU_RVA    0xfa6fd0u  /* returns the menu object */
#define HUD_ROW3_REGISTER_RVA 0xbfacc0u /* (registry, elem) -> bool */
#define HUD_ROW3_SLOTS_RVA   0x18a0490u /* static party slots, stride 0x18 */
#define HUD_ROW3_SLOT_STRIDE 0x18u
#define HUD_ROW3_SLOT_MAX    4
/* Where the row positions really come from (assigner 0x140c218ae..0x140c21919):
 *   root_elem   = *(void**)[0x14189f400]        ; registry[0], the HUD root element
 *   root_sprite = *(void**)(root_elem + 0x60)   ; its container sprite
 *   anchor(row) = root_sprite, group 0x5f for id 0x74 / 0x60 for id 0x75
 *   destination = *(void**)(row_elem + 0x60), group 0
 * So 0x5f/0x60 are indices in the ROOT sprite's name table (>= 96 entries), not
 * in a row sprite's table (37 entries) — which is why our row-sprite walk
 * refused them. Group 0 of a row's own container is the "…_ALL_pos" node the
 * game writes: 0x140dda850 -> 0x1405478f0 stores x/y/z at [entry+0xd0]+0x30/
 * 0x34/0x38 and ORs 0x10 into [entry+0xec] (dirty flag; without it the stored
 * value is never picked up — that was trial 10's inert write). */
#define HUD_ROW3_GROUP_ROW1  0x5f
#define HUD_ROW3_GROUP_ROW2  0x60
#define HUD_ROW3_GROUP_POS   0
#define HUD_ROW3_ENTRY_FLAGS 0xecu
#define HUD_ROW3_ENTRY_DIRTY 0x10u

/* The per-frame driver is not the element registry: rows 1/2 are updated by a
 * caller reached through heap (script) objects, so there is no static call site
 * to copy. What is reachable is the class's own vtable, and rows 1/2 go through
 * it every frame. Patching the two per-frame slots of the *static* vtable
 * (0x18 update, 0x50 frame) turns the game's own per-frame call into our tick;
 * our element keeps its private copy, so this cannot recurse into itself. */
#define HUD_ROW3_NATIVE_UPDATE_RVA 0xc1f500u
#define HUD_ROW3_NATIVE_FRAME_RVA  0xc210c0u

/* Companion candidates: the assigner (0xc21240, called from the class update at
 * 0xc1f8e4) walks a hash set of characters; the static party slot array is the
 * same population addressed by slot. Its acceptance test (0xc21330..0xc21360):
 *   char != own(slot 0), *(i32*)(char+0xef0) != -1, *(u8*)(char+0x430) != 0,
 *   *(void**)(char+0x240) != NULL, *(i32*)([char+0x240]+0x10) != 1. */
#define HUD_ROW3_SET_RVA     0x18a0590u /* {set, ?, ?, begin, count} */
#define HUD_ROW3_CHAR_DATA   0x240u     /* character +0x240 = player data */
#define HUD_ROW3_CHAR_VALID  0x430u     /* must be non-zero */
#define HUD_ROW3_CHAR_STATE  0xef0u     /* must not be -1 */
#define HUD_ROW3_DATA_ACTIVE 0x10u      /* player data +0x10 must not be 1 */

/* The seven class methods that read or write block A/B, in the order the
 * vtable stores them. The thunks below dispatch on one of these. */
enum { HUD_ROW3_DTOR, HUD_ROW3_INIT, HUD_ROW3_RELEASE, HUD_ROW3_UPDATE,
       HUD_ROW3_FRAME, HUD_ROW3_B8, HUD_ROW3_D8, HUD_ROW3_SLOT_COUNT };

/* The two methods in the last two slots take a bool in dl; the rest take
 * (this) or (this, arg) only, so forwarding integer registers is enough. */
typedef void *(*hud_row_dtor_fn)(void *self, unsigned flags);
typedef int (*hud_row_bool_fn)(void *self);
typedef void (*hud_row_void_fn)(void *self);
typedef int (*hud_row_arg_fn)(void *self, void *arg, void *block);
typedef int (*hud_row_flag_fn)(void *self, unsigned char on);
typedef void *(*hud_menu_factory_fn)(void *menu, int size, void *args);
typedef void *(*hud_menu_get_fn)(void);
typedef int (*hud_register_fn)(void *registry, void *elem);

static uint8_t g_hud_row3_block[HUD_ROW3_BLOCK_SIZE] __attribute__((aligned(8)));   /* block C */
static uint8_t g_hud_row3_saved[HUD_ROW3_BLOCK_SIZE] __attribute__((aligned(8)));   /* block A, while swapped */
static void *g_hud_row3_vtable;
static void *g_hud_row3_orig[HUD_ROW3_SLOT_COUNT];
static volatile LONG g_hud_row3_swap;      /* 1 while A carries C */
static volatile LONG g_hud_row3_active;    /* thunks may swap */
static volatile LONG g_hud_row3_dead;      /* the game destroyed our element */
static void *g_hud_row3_elem;              /* the element we published */
static int g_hud_row3_attempts;            /* build attempts this session */
static int g_hud_row3_failed;              /* stop trying for this session */
static int g_hud_row3_settle;
static void *g_hud_row3_char;
static int g_hud_row3_char_slot = -1;
static volatile LONG g_hud_row3_pending;   /* worker -> frame thunk: place+show */
static volatile LONG g_hud_row3_frame_fired;
static int g_hud_row3_starved;
/* Piggyback on the native rows' per-frame slots. */
static void *g_hud_row3_native_orig[2];    /* 0 = update (0x18), 1 = frame (0x50) */
static volatile LONG g_hud_row3_native_ok; /* slots patched, originals known */
static volatile LONG g_hud_row3_seen_update;
static volatile LONG g_hud_row3_seen_frame;
static volatile LONG g_hud_row3_driven;    /* at least one game-thread drive */
static DWORD g_hud_row3_last_drive;        /* rate limit, ms */
static int g_hud_row3_last_had_arg;        /* that tick included the update */
static DWORD g_hud_row3_lost_ms;           /* throttle for the churn log */
/* The prep/lobby screen builds rows 0x74/0x75 too, so the third row must wait
 * for the mission HUD. The mission loader phase ([manager+0x5d0] == 0xC, the
 * same field `rig.py state` prints as loader) separates the two. */
static volatile LONG g_hud_row_gen;        /* bumped for every native 0x74 build */
static int g_hud_row3_gen = -1;            /* generation our element belongs to */
static volatile LONG g_hud_row3_ticks;
static int g_hud_row3_gate_logged = -1;    /* last gate snapshot written */
static int g_hud_row3_stale_gen = -2;      /* generation last reported stale */
static int g_hud_row3_wait_gen = -2;       /* generation last reported elementless */

static uint8_t *hud_row3_a(void) { return g_base + HUD_ROW3_BLOCK_A_RVA; }
static uint8_t *hud_row3_b(void) { return g_base + HUD_ROW3_BLOCK_B_RVA; }

/* In the mission, not the prep/lobby screen that also builds 0x74/0x75. */
static int hud_row3_mission(void)
{
    uint8_t *const *manager = (uint8_t *const *)(g_base + GAME_MANAGER_RVA);
    if (!readable(manager, 8) || !*manager ||
        !readable(*manager + GAME_LOADER_OFF, 4))
        return 0;
    return *(const uint32_t *)(*manager + GAME_LOADER_OFF) == GAME_LOADER_IN_MISSION;
}

/* A gauge generation our element belongs to. Set when our element is built,
 * compared on every drive: after the HUD is rebuilt (mission entry, mode
 * change) the old element and its sprites are gone, and driving it crashes. */
static int hud_row3_generation(void)
{
    return (int)g_hud_row_gen;
}

static void **hud_row3_elements(void);
static void *hud_row3_elem_at(void **registry, int id);
static int hud_row3_native(void *elem);
static void hud_row3_place(void *row3, void *row1, void *row2);
static int piggy_update(void *self, void *arg, void *block);
static int piggy_frame(void *self);

/* Take block A out of row 1's hands and put C there.
 *   1  swapped in; caller must call hud_row3_leave()
 *   0  the swap is already held, i.e. a nested call: A already carries C
 *  -1  cannot swap, so the class must not run at all (it would operate on
 *      row 1's block) */
static int hud_row3_enter(void)
{
    if (!g_hud_row3_active)
        return -1;
    if (InterlockedCompareExchange(&g_hud_row3_swap, 1, 0) != 0)
        return 0;
    uint8_t *const a = hud_row3_a();
    if (!readable(a, HUD_ROW3_BLOCK_SIZE)) {
        InterlockedExchange(&g_hud_row3_swap, 0);
        return -1;
    }
    /* Session-scoped pointers, mirrored from the live block A on every swap.
     * An exhaustive scan of the class module (0xc1e000..0xc21a00) finds only
     * *writes* of 0 to +0x38/+0x40 (the resets at 0xc1eb58/0xc1eb5c and
     * 0xc1e646/0xc1e64a) and no read that dereferences them, yet every live
     * block in a mission carries non-zero values there (dumps: +0x38 heap,
     * +0x40 a pointer into the static party-slot table, e.g. 0x1418a04c0 =
     * &slot[2]). They are therefore written by another subsystem; mirroring
     * keeps C looking like a live block for whoever reads them.
     * Deliberately NOT mirrored: +0x00 state, +0x08..+0x20 HP/Ki, +0x30
     * companion, +0x48..+0x60 sprites, +0x68 fade timer — those are this
     * row's own state and would otherwise inherit row 1's. */
    *(void **)(g_hud_row3_block + 0x38) = *(void *const *)(a + 0x38);
    *(void **)(g_hud_row3_block + 0x40) = *(void *const *)(a + 0x40);
    memcpy(g_hud_row3_saved, a, HUD_ROW3_BLOCK_SIZE);
    memcpy(a, g_hud_row3_block, HUD_ROW3_BLOCK_SIZE);
    return 1;
}

/* Harvest whatever the call left in A back into C, then put row 1's block
 * back. Nothing else may run between the two halves on this thread. */
static void hud_row3_leave(void)
{
    uint8_t *const a = hud_row3_a();
    if (readable(a, HUD_ROW3_BLOCK_SIZE)) {
        memcpy(g_hud_row3_block, a, HUD_ROW3_BLOCK_SIZE);
        memcpy(a, g_hud_row3_saved, HUD_ROW3_BLOCK_SIZE);
    }
    InterlockedExchange(&g_hud_row3_swap, 0);
}

/* A call into our element's own vtable from the game must obey the same
 * freshness rule as the drive: after a HUD rebuild the element and its sprites
 * belong to the previous gauge generation and the class must not run. */
static int hud_row3_self_stale(void *self)
{
    return self == g_hud_row3_elem && g_hud_row3_gen != hud_row3_generation();
}
/* Every failure path after the block swap has to drop the active flag: with it
 * left at 1 the drive and the frame thunks keep treating block C as live while
 * no element is registered, and the next attempt builds on top of a stale C.
 * The element is not released: the proxy knows no deleter for the menu's
 * children, and inventing a vtable call into game code is worse than at most
 * one leaked 0x68-byte element per attempt (capped at three). */
static void hud_row3_abandon(void)
{
    InterlockedExchange(&g_hud_row3_active, 0);
    g_hud_row3_elem = NULL;
    g_hud_row3_gen = -1;
    g_hud_row3_char = NULL;
    g_hud_row3_char_slot = -1;
}

static void *thunk_dtor(void *self, unsigned flags)
{
    const int st = hud_row3_enter();
    if (st < 0)
        return NULL;
    void *const r = ((hud_row_dtor_fn)g_hud_row3_orig[HUD_ROW3_DTOR])(self, flags);
    if (st)
        hud_row3_leave();
    /* The object (and its sprites) are gone; the registry slot is cleared on
     * the next tick so nothing dereferences freed memory. */
    if (self == g_hud_row3_elem) {
        InterlockedExchange(&g_hud_row3_dead, 1);
        LOG("hud row3: the game destroyed the third row, it will be recreated");
    }
    return r;
}

static int thunk_init(void *self)
{
    const int st = hud_row3_enter();
    if (st < 0)
        return 0;
    const int r = ((hud_row_bool_fn)g_hud_row3_orig[HUD_ROW3_INIT])(self);
    if (st)
        hud_row3_leave();
    return r;
}

static void thunk_release(void *self)
{
    const int st = hud_row3_enter();
    if (st < 0)
        return;
    ((hud_row_void_fn)g_hud_row3_orig[HUD_ROW3_RELEASE])(self);
    if (st)
        hud_row3_leave();
}

/* Slot 0x18 takes (this, per-frame arg, r8). r8 is NOT the state block: in the
 * field it is 0x140bfb5a0 on every client, an orphan function with no
 * references, i.e. dead register content the update never reads (it picks its
 * own block from the element id). It is forwarded verbatim so the call is
 * byte-identical to the game's, including that leftover value. */
static int thunk_update(void *self, void *arg, void *block)
{
    if (hud_row3_self_stale(self))
        return 0;
    const int st = hud_row3_enter();
    if (st < 0)
        return 0;
    const int r = ((hud_row_arg_fn)g_hud_row3_orig[HUD_ROW3_UPDATE])(self, arg, block);
    if (st)
        hud_row3_leave();
    return r;
}

static int thunk_frame(void *self)
{
    if (hud_row3_self_stale(self))
        return 0;
    const int st = hud_row3_enter();
    if (st < 0)
        return 0;
    const int r = ((hud_row_bool_fn)g_hud_row3_orig[HUD_ROW3_FRAME])(self);
    if (st)
        hud_row3_leave();
    /* The first game-thread calls of our element answer two open questions at
     * once: the game drives registry-only rows, and placement may call the
     * game's anchor helpers, which is only safe on this thread. */
    if (self == g_hud_row3_elem) {
        if (!InterlockedExchange(&g_hud_row3_frame_fired, 1))
            LOG("hud row3: the game drives the frame slot on the game thread");
        if (InterlockedCompareExchange(&g_hud_row3_pending, 0, 1) != 0) {
            void **const registry = hud_row3_elements();
            void *const row1 = hud_row3_elem_at(registry, 0x74);
            void *const row2 = hud_row3_elem_at(registry, 0x75);
            if (hud_row3_native(row1) && hud_row3_native(row2)) {
                static int shown;
                LOG("hud row3: thunk_frame: pending place from the game thread "
                    "(row1 %p row2 %p self %p)", row1, row2, self);
                hud_row3_place(self, row1, row2);
                LOG("hud row3: thunk_frame: pending place returned");
                hud_row_show(self);
                if (!shown) {
                    shown = 1;
                    LOG("hud row3: shown, state word %#x, container sprite %p",
                        *(const uint32_t *)((const uint8_t *)self + HUD_ROW_FLAGS_OFF),
                        *(void *const *)((const uint8_t *)self + HUD_ROW3_CONTAINER_OFF));
                }
            }
        }
    }
    return r;
}

static int thunk_b8(void *self, unsigned char on)
{
    const int st = hud_row3_enter();
    if (st < 0)
        return 0;
    const int r = ((hud_row_flag_fn)g_hud_row3_orig[HUD_ROW3_B8])(self, on);
    if (st)
        hud_row3_leave();
    return r;
}

static int thunk_d8(void *self, unsigned char on)
{
    const int st = hud_row3_enter();
    if (st < 0)
        return 0;
    const int r = ((hud_row_flag_fn)g_hud_row3_orig[HUD_ROW3_D8])(self, on);
    if (st)
        hud_row3_leave();
    return r;
}

/* Copy the class vtable once and point the seven block-touching slots at the
 * thunks. Slots are matched by the method address, so a shifted table cannot
 * silently redirect the wrong method; anything unrecognised stays original. */
static int hud_row3_vtable_setup(void)
{
    if (g_hud_row3_vtable)
        return 1;
    const uint8_t *const src = g_base + HUD_ROW3_VTABLE_RVA;
    if (!readable(src, HUD_ROW3_VTABLE_SIZE))
        return 0;
    void **const copy = VirtualAlloc(NULL, HUD_ROW3_VTABLE_SIZE,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!copy)
        return 0;
    memcpy(copy, src, HUD_ROW3_VTABLE_SIZE);
    /* Slots are named by index *and* checked against the pristine method, so a
     * shifted table is refused instead of redirected. The pristine address (not
     * whatever the table holds) is what our thunks call, so the piggyback patch
     * on the static table can never leak into this copy or into the originals. */
    static const struct {
        int slot;
        uint32_t rva;
        int index;
        void *detour;
    } map[] = {
        {0x00, 0xc1e2d0u, HUD_ROW3_DTOR,    (void *)thunk_dtor},
        {0x08, 0xc1eba0u, HUD_ROW3_INIT,    (void *)thunk_init},
        {0x10, 0xc1e9c0u, HUD_ROW3_RELEASE, (void *)thunk_release},
        {0x18, 0xc1f500u, HUD_ROW3_UPDATE,  (void *)thunk_update},
        {0x50, 0xc210c0u, HUD_ROW3_FRAME,   (void *)thunk_frame},
        {0xb8, 0xc1f040u, HUD_ROW3_B8,      (void *)thunk_b8},
        {0xd8, 0xc1f140u, HUD_ROW3_D8,      (void *)thunk_d8},
    };
    int installed = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        const unsigned slot = (unsigned)map[i].slot / 8;
        void *const pristine = g_base + map[i].rva;
        void *const found = copy[slot];
        if (found != pristine && found != (void *)piggy_update &&
            found != (void *)piggy_frame) {
            LOG("hud row3: slot %#x holds %p, expected %p; refusing to thunk",
                map[i].slot, found, pristine);
            break;
        }
        g_hud_row3_orig[map[i].index] = pristine;
        copy[slot] = map[i].detour;
        installed++;
        LOG("hud row3: thunk at slot %#x (%#x), original %p",
            map[i].slot, map[i].rva, pristine);
    }
    if (installed != (int)(sizeof map / sizeof map[0])) {
        LOG("hud row3: only %d/%d thunks installed, giving up",
            installed, (int)(sizeof map / sizeof map[0]));
        VirtualFree(copy, 0, MEM_RELEASE);
        return 0;
    }
    g_hud_row3_vtable = copy;
    LOG("hud row3: private vtable copied (%#x bytes, id getter at +0xa8 left original)",
        HUD_ROW3_VTABLE_SIZE);
    return 1;
}

static void **hud_row3_elements(void)
{
    void **const holder = (void **)(g_base + HUD_ROW3_ELEMS_RVA);
    if (!readable(holder, sizeof *holder) || !*holder)
        return NULL;
    return (void **)*holder;
}

static void *hud_row3_elem_at(void **registry, int id)
{
    if (!registry || id < 0 || id >= 0x137)
        return NULL;
    if (!readable((uint8_t *)registry + (size_t)id * 8, sizeof(void *)))
        return NULL;
    return registry[id];
}

/* Our element, still ours: private vtable and the id we created it with. */
static int hud_row3_ours(void *elem)
{
    if (!elem || !g_hud_row3_vtable || !readable(elem, 0x68))
        return 0;
    return *(void **)elem == g_hud_row3_vtable &&
           *(const uint32_t *)((const uint8_t *)elem + HUD_ROW_ID_OFF) == HUD_ROW3_ID;
}

/* A native row: the class vtable, not our copy, and one of the two ids. */
static int hud_row3_native(void *elem)
{
    if (!elem || !readable(elem, 0x68))
        return 0;
    if (*(void **)elem != g_base + HUD_ROW3_VTABLE_RVA)
        return 0;
    const uint32_t id = *(const uint32_t *)((const uint8_t *)elem + HUD_ROW_ID_OFF);
    return id == 0x74 || id == 0x75;
}

static int hud_row3_sprite_count(const uint8_t *block)
{
    int n = 0;
    for (int i = 0; i < 4; i++)
        if (*(void *const *)(block + HUD_ROW3_SPRITES_OFF + (size_t)i * 8))
            n++;
    return n;
}

/* Create and initialise the third row. Returns the element or NULL; every
 * failure is logged with the step that produced it. */
static void *hud_row3_build(void **registry)
{
    if (!hud_row3_vtable_setup())
        return NULL;
    void *const menu = ((hud_menu_get_fn)(g_base + HUD_ROW3_MENU_RVA))();
    if (!menu || !readable(menu, 8)) {
        LOG("hud row3: menu object unavailable (%p), not creating", menu);
        return NULL;
    }
    void **const menu_vt = *(void ***)menu;
    if (!readable(menu_vt, 0x30 * 8) || !menu_vt[0x28 / 8]) {
        LOG("hud row3: menu factory slot missing (vt %p)", (void *)menu_vt);
        return NULL;
    }
    /* Stack struct the builder passes as the factory's third argument: the
     * live block at 0x140b3d1f5 writes {int 0x2d, qword param} and leaves the
     * four pad bytes alone, so they are zeroed here. */
    struct { int32_t tag; int32_t pad; uintptr_t param; } args;
    memset(&args, 0, sizeof args);
    args.tag = 0x2d;
    void *const elem = ((hud_menu_factory_fn)menu_vt[0x28 / 8])(menu, 0x68, &args);
    if (!elem) {
        LOG("hud row3: factory returned no element (size 0x68)");
        return NULL;
    }
    LOG("hud row3: element allocated at %p (menu %p)", elem, menu);

    memset(g_hud_row3_block, 0, sizeof g_hud_row3_block);
    g_hud_row3_char = NULL;
    g_hud_row3_char_slot = -1;
    InterlockedExchange(&g_hud_row3_active, 1);
    const int st = hud_row3_enter();        /* A carries C from here on */
    if (st < 0) {
        LOG("hud row3: cannot swap block A for block C, not creating");
        hud_row3_abandon();
        return NULL;
    }
    g_hud_row_ctor(elem, HUD_ROW3_ID, 0);   /* resets and binds "A" == C */
    *(void **)elem = g_hud_row3_vtable;     /* private vtable, id getter original */
    /* vt[0x08] is the init that creates the five sprites; run it through the
     * private vtable so the swap path is exercised, not just assumed. The
     * thunk sees the swap already held and calls the original directly. */
    const int inited = ((hud_row_bool_fn)((void **)g_hud_row3_vtable)[0x08 / 8])(elem);
    if (st)
        hud_row3_leave();
    if (!inited) {
        LOG("hud row3: init reported failure, element left unregistered");
        hud_row3_abandon();
        return NULL;
    }
    LOG("hud row3: init done, block C carries %d/4 sprites (+0x48..+0x60)",
        hud_row3_sprite_count(g_hud_row3_block));

    /* Published before registration: the game can call a freshly registered
     * element's slots within milliseconds (trial 3: the frame slot fired 10 ms
     * after registration), and both the drive and the thunks compare the
     * generation. A -1 there would look stale and drop a healthy element. */
    g_hud_row3_elem = elem;
    g_hud_row3_gen = hud_row3_generation();

    const int registered = ((hud_register_fn)(g_base + HUD_ROW3_REGISTER_RVA))(registry, elem);
    if (!registered) {
        LOG("hud row3: registration refused (slot %#x busy?), not showing",
            HUD_ROW3_ID);
        hud_row3_abandon();
        return NULL;
    }
    LOG("hud row3: registered in element slot %#x, gauge generation %d",
        HUD_ROW3_ID, g_hud_row3_gen);
    g_hud_row3_attempts = 0;    /* only consecutive failures count */
    ((hud_row_void_fn)((void **)g_hud_row3_vtable)[0x70 / 8])(elem);
    if (*(uint32_t *)((uint8_t *)elem + 0x50) != 0)
        LOG("hud row3: element flags +0x50 = %#x (expected 0)",
            *(uint32_t *)((uint8_t *)elem + 0x50));
    /* Visible only once a companion is bound: the tick does that, so an empty
     * third row never appears on screen. */
    return elem;
}

/* The character acceptance test of the assigner at 0xc21240 (0xc21330..60):
 * skip a character whose +0x430 is zero, whose +0xef0 is -1, whose player data
 * at +0x240 is null, or whose player data +0x10 is 1 (0x1407b4290 is exactly
 * "cmp $1,(%rcx)"). Missing the +0x430 direction made the rebind reject every
 * companion once the mission marked them valid. */
static int hud_row3_usable(void *ch)
{
    if (!ch)
        return 0;
    const uint8_t *const c = ch;
    if (!readable(c + HUD_ROW3_CHAR_VALID, 1) ||
        !readable(c + HUD_ROW3_CHAR_STATE, 4) ||
        !readable(c + HUD_ROW3_CHAR_DATA, 8))
        return 0;
    if (*(const uint8_t *)(c + HUD_ROW3_CHAR_VALID) == 0)
        return 0;
    if (*(const int32_t *)(c + HUD_ROW3_CHAR_STATE) == -1)
        return 0;
    const uint8_t *const data = *(const uint8_t *const *)(c + HUD_ROW3_CHAR_DATA);
    if (!data || !readable(data + HUD_ROW3_DATA_ACTIVE, 4))
        return 0;
    if (*(const int32_t *)(data + HUD_ROW3_DATA_ACTIVE) == 1)
        return 0;
    return 1;
}

/* One entry of the character set the assigner walks: it iterates the index
 * range [begin, begin+count) of an open-addressing table and takes the node's
 * first qword as the character. */
static void *hud_row3_char_from_set(int index)
{
    const uint8_t *const container = g_base + HUD_ROW3_SET_RVA;
    if (!readable(container, 0x28))
        return NULL;
    const uintptr_t begin = *(const uintptr_t *)(container + 0x18);
    const uintptr_t count = *(const uintptr_t *)(container + 0x20);
    if (index < 0 || (uintptr_t)index >= count || count > 64)
        return NULL;
    const void *const set = *(void *const *)container;
    if (!set || !readable(set, 8))
        return NULL;
    const uint8_t *const table = *(const uint8_t *const *)set;
    if (!table || !readable(table + 8, 0x10))
        return NULL;
    const uint64_t mask = *(const uint64_t *)(table + 0x10);
    void *const *const buckets = *(void *const *const *)(table + 8);
    if (!mask || !buckets)
        return NULL;
    const uint64_t slot = (begin + (uintptr_t)index) & (mask - 1);
    if (!readable(&buckets[slot], sizeof(void *)))
        return NULL;
    const void *const node = buckets[slot];
    if (!node || !readable(node, sizeof(void *)))
        return NULL;
    return *(void *const *)node;
}

/* Pick a companion neither native row is showing. Returns it and reports where
 * it came from: *slot_out >= 0 for the static party slot array, otherwise
 * *set_out is the character-set index. */
static void *hud_row3_pick(int *slot_out, int *set_out)
{
    *slot_out = -1;
    *set_out = -1;
    void *const used1 = *(void *const *)(hud_row3_a() + HUD_ROW3_CHAR_OFF);
    void *const used2 = *(void *const *)(hud_row3_b() + HUD_ROW3_CHAR_OFF);
    void *own = NULL;
    if (readable(g_base + HUD_ROW3_SLOTS_RVA, 8))
        own = *(void *const *)(g_base + HUD_ROW3_SLOTS_RVA);   /* slot 0 = own */
    static const int order[] = {3, 2, 1};
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) {
        const int slot = order[i];
        const uint8_t *const rec = g_base + HUD_ROW3_SLOTS_RVA +
                                   (size_t)slot * HUD_ROW3_SLOT_STRIDE;
        if (!readable(rec, 8))
            continue;
        void *const ch = *(void *const *)rec;
        if (!ch || ch == own || ch == used1 || ch == used2)
            continue;
        if (!hud_row3_usable(ch))
            continue;
        *slot_out = slot;
        LOG("hud row3: picked companion %p from party slot %d", ch, slot);
        return ch;
    }
    LOG("hud row3: no party-slot candidate; walking the companion set");
    for (int index = 0; index < 64; index++) {
        void *const ch = hud_row3_char_from_set(index);
        if (!ch || ch == own || ch == used1 || ch == used2)
            continue;
        if (!hud_row3_usable(ch))
            continue;
        *set_out = index;
        LOG("hud row3: picked companion %p from the set at index %d", ch, index);
        return ch;
    }
    return NULL;
}

/* Keep block C bound to a companion no native row shows. Runs on whichever
 * thread drives it; the block write is one aligned pointer store. */
static int hud_row3_maintain(void)
{
    void **const a = (void **)(hud_row3_a() + HUD_ROW3_CHAR_OFF);
    void **const b = (void **)(hud_row3_b() + HUD_ROW3_CHAR_OFF);
    void **const c = (void **)(g_hud_row3_block + HUD_ROW3_CHAR_OFF);
    uint32_t *const state = (uint32_t *)(g_hud_row3_block + HUD_ROW3_STATE_OFF);
    if (!readable(a, 8) || !readable(b, 8))
        return *c != NULL;
    const int had = g_hud_row3_char != NULL;

    if (*c && (*c == *a || *c == *b)) {
        /* The assigner re-parks companions every frame, so this can repeat;
         * one line per second still shows the churn without flooding the log. */
        const DWORD now = GetTickCount();
        if (now - g_hud_row3_lost_ms > 1000) {
            g_hud_row3_lost_ms = now;
            LOG("hud row3: companion slot %d (%p) taken by a native row, rebinding",
                g_hud_row3_char_slot, *c);
        }
        *c = NULL;
        *state = 0;
        g_hud_row3_char = NULL;
        g_hud_row3_char_slot = -1;
    } else if (*c && !hud_row3_usable(*c)) {
        LOG("hud row3: companion slot %d (%p) no longer usable, rebinding",
            g_hud_row3_char_slot, *c);
        *c = NULL;
        *state = 0;
        g_hud_row3_char = NULL;
        g_hud_row3_char_slot = -1;
    }

    if (!*c) {
        int slot = -1, set_index = -1;
        void *const ch = hud_row3_pick(&slot, &set_index);
        if (!ch)
            return 0;
        *c = ch;
        if (*state == 0)
            *state = HUD_ROW3_STATE_READY;
        g_hud_row3_char = ch;
        g_hud_row3_char_slot = slot >= 0 ? slot : -(set_index + 1);
        if (slot >= 0)
            LOG("hud row3: companion slot %d character %p %s block C (state %d)",
                slot, ch, had ? "rebound to" : "bound to", (int)*state);
        else
            LOG("hud row3: companion set[%d] character %p %s block C (state %d)",
                set_index, ch, had ? "rebound to" : "bound to", (int)*state);
    }
    return *c != NULL;
}

/* Row 3 is placed one row-pitch below row 2, computed from the two native
 * rows' own transforms and written straight into our container sprite's
 * transform. NO game calls: the 0x140c58930 read and the 0x140dda790 write
 * faulted in trial 6 when entered from inside our element's frame-slot call,
 * so the whole chain below is memory reads and three float stores.
 *
 * Chain (0x140dd8a90/0x140dd8b.. resolved from the runtime image):
 *   id     = *(u32*)sprite                                   (sprite id, +0x00)
 *   index  = rb_lookup(data_mgr[0x50].root, key id)   key u32@+0x1c, val u32@+0x20
 *   obj    = rb_lookup(inst_mgr[0x00].root, key index) key u32@+0x20, val ptr@+0x28
 *            (data_mgr = [0x141766128], inst_mgr = [0x141766110]; both trees are
 *             red-black with left@+0x00, right@+0x10, nil byte@+0x19 and the
 *             root at header+0x08 — this is 0x140f90a70 followed by 0x140f90930)
 *   holder = *(void**)(obj + 0xd8)
 *   table  = *(void**)(holder + 0x428)          count = u32@table+4
 *   key    = (char*)(table + u32@(table + (group+2)*4))   NUL-terminated group name
 *   C      = *(void**)(*(void**)(sprite + 0x28) + 0x28)   named-node container
 *   entry  = [[list_node+0x10]] for the list node under C+0x28 (next @+0x08,
 *            sentinel C+0x48) whose [[node+0x10]+0xa8] string equals key
 *   handle = *(void**)(entry+0xd0)   (only when *(u64*)(entry+0xb8) == 0; the
 *            other branch needs entry->vt[0xb0](entry,0), so it is refused)
 *   vec3   = floats at handle+0x30 (x), +0x34 (y), +0x38 (z)  [0x140dd8db5..] */
#define HUD_ROW3_MGR_DATA_RVA   0x1766128u  /* [.] data manager: sprite id -> index */
#define HUD_ROW3_MGR_INST_RVA   0x1766110u  /* [.] instance manager: index -> obj */
#define HUD_ROW3_CONTAINER_ID   0x27761u

/* Red-black lower_bound shared by both sprite-manager trees. Pure reads with a
 * depth cap: a torn tree yields "not found", never a wild pointer. */
static const uint8_t *hud_row3_tree_lower(const uint8_t *header, uint32_t key, int key_off)
{
    if (!header || !readable(header, 0x18))
        return NULL;
    const uint8_t *cur = *(const uint8_t *const *)(header + 8);
    const uint8_t *best = NULL;
    for (int depth = 0; depth < 128; depth++) {
        if (!cur || !readable(cur, 0x30))
            break;
        if (*(const uint8_t *)(cur + 0x19))
            break;                                  /* nil leaf */
        if (*(const uint32_t *)(cur + key_off) >= key) {
            best = cur;
            cur = *(const uint8_t *const *)cur;      /* left */
        } else {
            cur = *(const uint8_t *const *)(cur + 0x10);   /* right */
        }
    }
    if (!best || *(const uint32_t *)(best + key_off) != key)
        return NULL;   /* lower_bound + equality, as 0x140f90a70 does */
    return best;
}

/* The sprite-data object for a sprite, keyed by its id (two tree searches). */
static void *hud_row3_sprite_data(const void *sprite)
{
    if (!sprite || !readable(sprite, 8))
        return NULL;
    const uint32_t id = *(const uint32_t *)sprite;
    const void *const *const data_mgr = (const void *const *)(g_base + HUD_ROW3_MGR_DATA_RVA);
    const void *const *const inst_mgr = (const void *const *)(g_base + HUD_ROW3_MGR_INST_RVA);
    if (!readable(data_mgr, 8) || !*data_mgr || !readable(inst_mgr, 8) || !*inst_mgr)
        return NULL;
    if (!readable((const uint8_t *)*data_mgr + 0x50, 8))
        return NULL;
    const uint8_t *const header1 = *(const uint8_t *const *)((const uint8_t *)*data_mgr + 0x50);
    const uint8_t *const n1 = hud_row3_tree_lower(header1, id, 0x1c);
    if (!n1)
        return NULL;
    const uint32_t index = *(const uint32_t *)(n1 + 0x20);
    if (!readable(*inst_mgr, 8))
        return NULL;
    const uint8_t *const header2 = *(const uint8_t *const *)*inst_mgr;
    const uint8_t *const n2 = hud_row3_tree_lower(header2, index, 0x20);
    if (!n2)
        return NULL;
    return *(void *const *)(n2 + 0x28);
}

/* Trace of one chain walk. Filled only for the first few placement attempts so
 * a broken link names itself in the log instead of failing silently. */
typedef struct {
    void *obj, *holder, *table, *container, *handle;
    uint32_t count, off;
    const char *key;
    int nodes, named, matched;
    const char *fail;
} hud_row3_trace;

/* The transform object behind (sprite, group), or NULL; `tr` (optional) records
 * every intermediate pointer and the step that refused.
 *
 * Key arithmetic from 0x140dd6ce0 (0x140dd6d13..0x140dd6d2e):
 *   count = u32@table+4
 *   off   = u32@(table + (group+2)*4)
 *   name  = (count+2)*4 + off  BYTES FROM `table`, i.e. the offset is relative
 *           to the string pool that starts right after the offset array, not to
 *           the table base. Using table+off reads the offset array itself and
 *           the name match can never succeed — that was trial 8's silence. */
static void *hud_row3_transform_tr(const void *sprite, int group, hud_row3_trace *tr,
                                   void **entry_out)
{
    if (!sprite || !readable(sprite, 8)) {
        if (tr) tr->fail = "sprite";
        return NULL;
    }
    const uint8_t *const obj = hud_row3_sprite_data(sprite);
    if (tr) tr->obj = (void *)obj;
    if (!obj) {
        if (tr) tr->fail = "sprite-data (id lookup)";
        return NULL;
    }
    if (!readable(obj + 0xd8, 8)) {
        if (tr) tr->fail = "holder read";
        return NULL;
    }
    const uint8_t *const holder = *(const uint8_t *const *)(obj + 0xd8);
    if (tr) tr->holder = (void *)holder;
    if (!holder || !readable(holder + 0x428, 8)) {
        if (tr) tr->fail = "holder ([obj+0xd8])";
        return NULL;
    }
    const uint8_t *const table = *(const uint8_t *const *)(holder + 0x428);
    if (tr) tr->table = (void *)table;
    if (!table || !readable(table, 8)) {
        if (tr) tr->fail = "table ([holder+0x428])";
        return NULL;
    }
    const uint32_t count = *(const uint32_t *)(table + 4);
    if (tr) tr->count = count;
    if (group < 0 || (uint32_t)group >= count || count > 0x4000) {
        if (tr) tr->fail = "count (group >= count)";
        return NULL;
    }
    const uint32_t off = *(const uint32_t *)(table + (size_t)(group + 2) * 4);
    if (tr) tr->off = off;
    const uint8_t *const pool = table + (size_t)(count + 2) * 4;
    const char *const name = (const char *)(pool + off);
    if (tr) tr->key = name;
    if (!readable(name, 1) || !readable(name, 32)) {
        if (tr) tr->fail = "name read";
        return NULL;
    }
    {
        int nul = 0;
        for (int i = 0; i < 32; i++)
            if (!name[i])
                nul = 1;
        if (!nul) {
            if (tr) tr->fail = "name not NUL-terminated";
            return NULL;
        }
    }
    if (!readable((const uint8_t *)sprite + 0x28, 8)) {
        if (tr) tr->fail = "link read";
        return NULL;
    }
    const uint8_t *const link = *(const uint8_t *const *)((const uint8_t *)sprite + 0x28);
    if (!link || !readable(link + 0x28, 8)) {
        if (tr) tr->fail = "link ([sprite+0x28])";
        return NULL;
    }
    const uint8_t *const container = *(const uint8_t *const *)(link + 0x28);
    if (tr) tr->container = (void *)container;
    if (!container || !readable(container + 0x48, 8)) {
        if (tr) tr->fail = "container ([[sprite+0x28]+0x28])";
        return NULL;
    }
    const uint8_t *head = *(const uint8_t *const *)(container + 0x28);
    const uint8_t *const tail = container + 0x48;
    for (int depth = 0; depth < 256; depth++) {
        if (!head || head == tail || !readable(head, 0x18))
            break;
        if (tr) tr->nodes++;
        const uint8_t *const node = *(const uint8_t *const *)(head + 0x10);
        if (node && readable(node + 0xa8, 8)) {
            const char *const cand = *(const char *const *)(node + 0xa8);
            if (cand && readable(cand, 32)) {
                if (tr) tr->named++;
                int i = 0;
                while (i < 32 && cand[i] && cand[i] == name[i])
                    i++;
                if (i < 32 && cand[i] == name[i]) {
                    if (tr) tr->matched = 1;
                    if (!readable(node + 0xb8, 0x20)) {
                        if (tr) tr->fail = "entry fields";
                        return NULL;
                    }
                    if (*(const uint64_t *)(node + 0xb8) != 0) {
                        if (tr) tr->fail = "handle branch (+0xb8 != 0, needs a virtual call)";
                        return NULL;
                    }
                    const void *const handle = *(void *const *)(node + 0xd0);
                    if (tr) tr->handle = (void *)handle;
                    if (!handle) {
                        if (tr) tr->fail = "handle ([node+0xd0])";
                        return NULL;
                    }
                    if (entry_out)
                        *entry_out = (void *)node;
                    return (void *)handle;
                }
            }
        }
        head = *(const uint8_t *const *)(head + 0x08);
    }
    if (tr) tr->fail = "name match in the container list";
    return NULL;
}

/* One log line per walked chain, used for the first three placement attempts. */
static void hud_row3_trace_line(const char *who, const void *sprite, int group,
                                const hud_row3_trace *t)
{
    char name[40];
    int i = 0;
    name[0] = 0;
    if (t->key && readable(t->key, 1)) {
        for (; i < (int)sizeof name - 1; i++) {
            if (!readable(t->key + i, 1) || !t->key[i])
                break;
            const char c = t->key[i];
            name[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
        }
    }
    name[i] = 0;
    LOG("hud row3: chain %s sprite %p group %#x: obj %p holder %p table %p "
        "count %u off %#x key %p \"%s\" container %p nodes %d named %d matched %d "
        "handle %p, failed at %s", who, sprite, group, t->obj, t->holder, t->table,
        t->count, t->off, (void *)t->key, name, t->container, t->nodes, t->named,
        t->matched, t->handle, t->fail ? t->fail : "nothing");
}

/* The chain is pure reads, so a wrong offset yields a wrong number rather than
 * a fault. Refuse anything that is not a plausible screen coordinate before it
 * can be written anywhere. */
static int hud_row3_vec_sane(const float v[3])
{
    for (int i = 0; i < 3; i++) {
        if (v[i] != v[i])                       /* NaN */
            return 0;
        if (v[i] > 1.0e6f || v[i] < -1.0e6f)
            return 0;
    }
    return 1;
}

/* The HUD root element (registry[0]) and its container sprite: the assigner
 * reads the 0x5f/0x60 anchors from that sprite, not from a row's sprite. */
static void *hud_row3_root_sprite(void)
{
    const void *const *const holder = (const void *const *)(g_base + HUD_ROW3_ELEMS_RVA);
    if (!readable(holder, 8) || !*holder)
        return NULL;
    const uint8_t *const root = *(const uint8_t *const *)*holder;
    if (!root || !readable(root + HUD_ROW3_CONTAINER_OFF, 8))
        return NULL;
    void *const sprite = *(void **)(root + HUD_ROW3_CONTAINER_OFF);
    return readable(sprite, 8) ? sprite : NULL;
}

/* Read one anchor vector (raw, pre-UI-scale: 0x140dd8d50 divides by the scale
 * factors at the very end, and the write path multiplies them back, so raw
 * values from the same space are directly comparable and writable). */
static int hud_row3_anchor(void *sprite, int group, float out[3], void **entry_out)
{
    void *const handle = hud_row3_transform_tr(sprite, group, NULL, entry_out);
    if (!handle || !readable((uint8_t *)handle + 0x30, 0xc))
        return 0;
    out[0] = *(const float *)((const uint8_t *)handle + 0x30);
    out[1] = *(const float *)((const uint8_t *)handle + 0x34);
    out[2] = *(const float *)((const uint8_t *)handle + 0x38);
    return 1;
}

/* Store the position the way 0x1405478f0 does: the three floats at
 * [entry+0xd0]+0x30/0x34/0x38 plus the 0x10 bit in [entry+0xec], which is what
 * makes the UI pick the new transform up. */
static int hud_row3_store_pos(void *entry, const float v[3])
{
    if (!entry || !readable((uint8_t *)entry + 0xd0, 8) ||
        !readable((uint8_t *)entry + HUD_ROW3_ENTRY_FLAGS, 4))
        return 0;
    uint8_t *const handle = *(uint8_t *const *)((uint8_t *)entry + 0xd0);
    if (!handle || !readable(handle + 0x30, 0xc))
        return 0;
    *(float *)(handle + 0x30) = v[0];
    *(float *)(handle + 0x34) = v[1];
    *(float *)(handle + 0x38) = v[2];
    *(uint32_t *)((uint8_t *)entry + HUD_ROW3_ENTRY_FLAGS) |= HUD_ROW3_ENTRY_DIRTY;
    return 1;
}

/* One row-pitch below row 2 on screen. Source and destination are the pair the
 * game's own assigner uses (see the comment above HUD_ROW3_GROUP_ROW1): the
 * 0x5f/0x60 anchors of the ROOT element's container sprite give rows 1/2, the
 * result goes into group 0 of OUR container, plus the 0x10 dirty bit that makes
 * the UI consume it. Written only when the value changes (or every 30th call). */
static void hud_row3_place(void *row3, void *row1, void *row2)
{
    (void)row1;
    (void)row2;
    static float last[3];
    static int have_last, since_write, logged;
    if (!readable(row3, HUD_ROW3_CONTAINER_OFF + 8))
        return;
    void *const sprite3 = *(void **)((uint8_t *)row3 + HUD_ROW3_CONTAINER_OFF);
    void *const root = hud_row3_root_sprite();
    float v1[3], v2[3], v3[3];
    /* First three attempts: walk the used chains and log every link, so a
     * broken step names itself (three lines, then silence). */
    static int probes;
    if (probes < 3) {
        hud_row3_trace t;
        probes++;
        if (root) {
            memset(&t, 0, sizeof t);
            hud_row3_transform_tr(root, HUD_ROW3_GROUP_ROW1, &t, NULL);
            hud_row3_trace_line("row1", root, HUD_ROW3_GROUP_ROW1, &t);
            memset(&t, 0, sizeof t);
            hud_row3_transform_tr(root, HUD_ROW3_GROUP_ROW2, &t, NULL);
            hud_row3_trace_line("row2", root, HUD_ROW3_GROUP_ROW2, &t);
        } else {
            LOG("hud row3: chain row1/row2: no root sprite (registry[0] %p)",
                (void *)hud_row3_elements());
        }
        memset(&t, 0, sizeof t);
        hud_row3_transform_tr(sprite3, HUD_ROW3_GROUP_POS, &t, NULL);
        hud_row3_trace_line("row3", sprite3, HUD_ROW3_GROUP_POS, &t);
    }
    if (!root) {
        if (!logged) {
            logged = 1;
            LOG("hud row3: place: no root sprite, row keeps its default position");
        }
        return;
    }
    void *entry1 = NULL, *entry2 = NULL;
    if (!hud_row3_anchor(root, HUD_ROW3_GROUP_ROW1, v1, &entry1) ||
        !hud_row3_anchor(root, HUD_ROW3_GROUP_ROW2, v2, &entry2) ||
        !hud_row3_vec_sane(v1) || !hud_row3_vec_sane(v2)) {
        if (!logged) {
            logged = 1;
            LOG("hud row3: place: root anchors not readable (row1 %d,%d row2 %d,%d)",
                (int)(v1[0] * 1000.0f), (int)(v1[1] * 1000.0f),
                (int)(v2[0] * 1000.0f), (int)(v2[1] * 1000.0f));
        }
        return;
    }
    for (int i = 0; i < 3; i++)
        v3[i] = v2[i] + (v2[i] - v1[i]);
    /* Our own entry must be a different node than the root anchors'. */
    void *entry3 = NULL;
    if (!hud_row3_transform_tr(sprite3, HUD_ROW3_GROUP_POS, NULL, &entry3) || !entry3) {
        if (!logged) {
            logged = 1;
            LOG("hud row3: place: our transform entry not resolvable (sprite %p)",
                sprite3);
        }
        return;
    }
    /* Never write through a node the anchors themselves live in. */
    if (entry3 == entry1 || entry3 == entry2) {
        if (!logged) {
            logged = 1;
            LOG("hud row3: place: our entry %p is a shared anchor node "
                "(0x5f %p, 0x60 %p); refusing to write", entry3, entry1, entry2);
        }
        return;
    }
    int changed = !have_last;
    for (int i = 0; i < 3 && !changed; i++)
        if (v3[i] > last[i] + 0.001f || v3[i] < last[i] - 0.001f)
            changed = 1;
    if (!changed && ++since_write < 30)
        return;
    if (!hud_row3_store_pos(entry3, v3)) {
        if (!logged) {
            logged = 1;
            LOG("hud row3: place: our entry not writable (entry %p)", entry3);
        }
        return;
    }
    have_last = 1;
    since_write = 0;
    for (int i = 0; i < 3; i++)
        last[i] = v3[i];
    if (!logged) {
        logged = 1;
        LOG("hud row3: position root 0x5f=(%d,%d) 0x60=(%d,%d) row3=(%d,%d) "
            "(x1000); wrote entry %p sprite %p + dirty flag",
            (int)(v1[0] * 1000.0f), (int)(v1[1] * 1000.0f),
            (int)(v2[0] * 1000.0f), (int)(v2[1] * 1000.0f),
            (int)(v3[0] * 1000.0f), (int)(v3[1] * 1000.0f), entry3, sprite3);
    }
}

/* Run the class's own update / frame code for our element, with block C in
 * place of A. This is what makes row 3 animate exactly like rows 1/2.
 * The update gets block A as its third argument: A carries C for the duration
 * of the call, and A is the block our element's id selects. */
static void hud_row3_run_update(void *row3, void *arg, void *block)
{
    const int st = hud_row3_enter();
    if (st < 0)
        return;
    /* Forward r8 exactly as the game passed it. It is not the state block: the
     * dump shows 0x140bfb5a0 there on every client, an orphan function address
     * with no references, i.e. dead register content the class update never
     * reads (it computes its own block from the element id). */
    ((hud_row_arg_fn)g_hud_row3_orig[HUD_ROW3_UPDATE])(row3, arg, block);
    if (st)
        hud_row3_leave();
}

static void hud_row3_run_frame(void *row3)
{
    const int st = hud_row3_enter();
    if (st < 0)
        return;
    ((hud_row_bool_fn)g_hud_row3_orig[HUD_ROW3_FRAME])(row3);
    if (st)
        hud_row3_leave();
}

/* One game-thread tick for row 3, taken from a native row's own per-frame call.
 * arg is whatever the game passed to that row's update slot; the same context
 * is what our element's update expects, and it is only available on the update
 * slot. */
static void hud_row3_drive(void *arg, void *block, int have_arg)
{
    if (!g_cfg.hud_row3 || g_cfg.max_players < 4 || g_hud_row3_failed ||
        !g_hud_row3_native_ok)
        return;
    if (g_hud_row3_swap)
        return;                     /* a swap is in flight, try next frame */
    if (g_hud_row3_dead)
        return;                     /* destroyed/stale: the worker rebuilds it */
    if (!hud_row_wanted() || !hud_row3_mission())
        return;                     /* prep/lobby or mode off */
    /* Freshness: an element recorded for an older gauge generation must never be
     * driven (its sprites died with the previous HUD). But "no element yet" is
     * NOT stale: the generation is -1 until the worker builds one, and treating
     * that as stale is what starved the build in trial 4 (drive set dead every
     * frame, the tick's cleanup reset the settle, and the build never ran). */
    const int gen = hud_row3_generation();
    if (g_hud_row3_elem) {
        if (g_hud_row3_gen != gen) {
            InterlockedExchange(&g_hud_row3_dead, 1);
            if (gen != g_hud_row3_stale_gen) {
                g_hud_row3_stale_gen = gen;
                LOG("hud row3: element is from gauge generation %d, now %d; "
                    "marking it stale (no drive)", g_hud_row3_gen, gen);
            }
            return;
        }
    } else if (gen != g_hud_row3_wait_gen) {
        g_hud_row3_wait_gen = gen;
        LOG("hud row3: drive is live in gauge generation %d with no element yet; "
            "waiting for the worker build", gen);
    }
    const DWORD now = GetTickCount();
    if (now == g_hud_row3_last_drive && (!have_arg || g_hud_row3_last_had_arg))
        return;                     /* this frame's tick already ran */
    g_hud_row3_last_drive = now;
    g_hud_row3_last_had_arg = have_arg;

    void **const registry = hud_row3_elements();
    if (!registry)
        return;
    void *const row1 = hud_row3_elem_at(registry, 0x74);
    void *const row2 = hud_row3_elem_at(registry, 0x75);
    if (!hud_row3_native(row1) || !hud_row3_native(row2))
        return;
    void *const row3 = hud_row3_elem_at(registry, HUD_ROW3_ID);
    if (!hud_row3_ours(row3)) {
        InterlockedExchange(&g_hud_row3_pending, 1);   /* worker builds it */
        return;
    }
    if (!hud_row3_maintain())
        return;                     /* no companion to show yet */
    void **const cslot = (void **)(g_hud_row3_block + HUD_ROW3_CHAR_OFF);
    void *const before = *cslot;
    /* One-shot stage logs: if a bound run faults, the last of these names the
     * stage (update / frame / place / show) and the callee it was entering. */
    static int s_bound, s_updated, s_framed, s_placed;
    if (!s_bound) {
        s_bound = 1;
        LOG("hud row3: drive: running the bound update (block C char %p, "
            "+0x38 %p, +0x40 %p, arg %p, r8 %p)", before,
            *(void *const *)(g_hud_row3_block + 0x38),
            *(void *const *)(g_hud_row3_block + 0x40), arg, block);
    }
    if (have_arg)
        hud_row3_run_update(row3, arg, block);
    if (!s_updated) {
        s_updated = 1;
        LOG("hud row3: drive: bound update returned");
    }
    /* The class update runs the assigner, which parks a companion into the
     * block its id selects. With A swapped to C that is C, but the assigner
     * cannot see row 1's real companion while the swap is held, so it may pick
     * that one. Drop a conflicting pick and keep what maintain() chose. */
    if (*cslot && (*cslot == *(void *const *)(hud_row3_a() + HUD_ROW3_CHAR_OFF) ||
                   *cslot == *(void *const *)(hud_row3_b() + HUD_ROW3_CHAR_OFF))) {
        *cslot = before;
        if (!before) {
            *cslot = NULL;
            *(uint32_t *)(g_hud_row3_block + HUD_ROW3_STATE_OFF) = 0;
            LOG("hud row3: the class update parked a duplicate companion; "
                "block C cleared");
            return;
        }
    }
    g_hud_row3_char = *cslot;
    hud_row3_run_frame(row3);
    if (!s_framed) {
        s_framed = 1;
        LOG("hud row3: drive: bound frame slot returned");
    }
    hud_row3_place(row3, row1, row2);
    if (!s_placed) {
        s_placed = 1;
        LOG("hud row3: drive: place returned");
    }
    hud_row_show(row3);
    if (!InterlockedExchange(&g_hud_row3_driven, 1))
        LOG("hud row3: per-frame drive confirmed through a native row (%s), "
            "state word %#x, container sprite %p", have_arg ? "update arg" : "frame slot",
            *(const uint32_t *)((const uint8_t *)row3 + HUD_ROW_FLAGS_OFF),
            *(void *const *)((const uint8_t *)row3 + HUD_ROW3_CONTAINER_OFF));
}

/* Piggyback thunks for the *static* vtable: they are entered for rows 1/2 on
 * the game thread and turn the game's own per-frame call into our tick. Our
 * element uses the private copy, so this never re-enters itself. */
static int piggy_update(void *self, void *arg, void *block)
{
    const int r = ((hud_row_arg_fn)g_hud_row3_native_orig[0])(self, arg, block);
    if (!InterlockedExchange(&g_hud_row3_seen_update, 1))
        LOG("hud row3: piggyback: native update slot (0x18) runs per frame "
            "(self %p, arg %p, r8 %p)", self, arg, block);
    hud_row3_drive(arg, block, 1);
    return r;
}

static int piggy_frame(void *self)
{
    const int r = ((hud_row_bool_fn)g_hud_row3_native_orig[1])(self);
    if (!InterlockedExchange(&g_hud_row3_seen_frame, 1))
        LOG("hud row3: piggyback: native frame slot (0x50) runs per frame (self %p)",
            self);
    hud_row3_drive(NULL, NULL, 0);
    return r;
}

/* Install the piggyback. Writes only the class vtable's own two slots in
 * .rdata; the originals are the game's method addresses, so a table that was
 * already patched is refused rather than chained. */
static int hud_row3_native_hook(void)
{
    if (g_hud_row3_native_ok) {
        /* A protector or a rebuild could restore the table; re-check cheaply. */
        void **const vt = (void **)(g_base + HUD_ROW3_VTABLE_RVA);
        if (!readable(vt, HUD_ROW3_VTABLE_SIZE) ||
            vt[0x18 / 8] != (void *)piggy_update ||
            vt[0x50 / 8] != (void *)piggy_frame) {
            LOG("hud row3: piggyback was reverted, reinstalling");
            InterlockedExchange(&g_hud_row3_native_ok, 0);
        } else {
            return 1;
        }
    }
    void **const vt = (void **)(g_base + HUD_ROW3_VTABLE_RVA);
    if (!readable(vt, HUD_ROW3_VTABLE_SIZE))
        return 0;
    void *const want[2] = {g_base + HUD_ROW3_NATIVE_UPDATE_RVA,
                           g_base + HUD_ROW3_NATIVE_FRAME_RVA};
    const int slot[2] = {0x18 / 8, 0x50 / 8};
    for (int i = 0; i < 2; i++) {
        if (vt[slot[i]] != want[i]) {
            LOG("hud row3: piggyback refused: slot %#x holds %p, expected %p",
                slot[i] * 8, vt[slot[i]], want[i]);
            return 0;
        }
    }
    DWORD old;
    if (!VirtualProtect(vt, HUD_ROW3_VTABLE_SIZE, PAGE_READWRITE, &old)) {
        LOG("hud row3: piggyback refused: cannot write the class vtable (err %lu)",
            GetLastError());
        return 0;
    }
    g_hud_row3_native_orig[0] = want[0];
    g_hud_row3_native_orig[1] = want[1];
    vt[slot[0]] = (void *)piggy_update;
    vt[slot[1]] = (void *)piggy_frame;
    VirtualProtect(vt, HUD_ROW3_VTABLE_SIZE, old, &old);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    /* Logged before the flag is published, so a piggy thunk's first-call line
     * can never precede this one in the log (the trial-3 ordering made it look
     * like the install never happened). */
    LOG("hud row3: piggyback installed on the native row slots 0x18 (update %#x) "
        "and 0x50 (frame %#x)", HUD_ROW3_NATIVE_UPDATE_RVA, HUD_ROW3_NATIVE_FRAME_RVA);
    InterlockedExchange(&g_hud_row3_native_ok, 1);
    return 1;
}


/* ------------------------------------------------- row3 fault reporter -- */

/* Trial 5-6 lost two rig cycles to a fault we could only name by log ordering.
 * This reports the faulting image offset once per position, then removes
 * itself and lets the process die exactly as it would have: no recovery, no
 * handler chaining tricks, just a precise address in the log. */
static PVOID g_hud_row3_veh;
static uintptr_t g_hud_row3_veh_seen[4];
static volatile LONG g_hud_row3_veh_n;

static size_t hud_row3_image_size(void)
{
    if (!g_base || !readable(g_base, 0x40))
        return 0;
    const uint32_t e = *(const uint32_t *)(g_base + 0x3c);
    if (e < 0x40 || e > 0x1000 || !readable(g_base + e, 0x40))
        return 0;
    if (*(const uint32_t *)(g_base + e) != 0x00004550u)   /* "PE\0\0" */
        return 0;
    return *(const uint32_t *)(g_base + e + 24 + 0x38);    /* OptionalHeader.SizeOfImage */
}

static LONG WINAPI hud_row3_veh(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *const er = ep->ExceptionRecord;
    const uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    const size_t size = hud_row3_image_size();
    int reported = 0;
    if (g_base && size && rip >= (uintptr_t)g_base &&
        rip < (uintptr_t)g_base + size) {
        const LONG n = g_hud_row3_veh_n;
        int seen = 0;
        for (LONG i = 0; i < n && i < 4; i++)
            if (g_hud_row3_veh_seen[i] == rip)
                seen = 1;
        if (!seen && n < 4) {
            g_hud_row3_veh_seen[n] = rip;
            InterlockedExchange(&g_hud_row3_veh_n, n + 1);
            const uintptr_t access =
                (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                    ? (uintptr_t)er->ExceptionInformation[1] : 0;
            LOG("hud row3: exception 0x%08x at exe+%#x (access addr %p, rip %p)",
                (unsigned)er->ExceptionCode, (unsigned)(rip - (uintptr_t)g_base),
                (void *)access, (void *)rip);
            reported = 1;
        }
    }
    /* Only an in-image fault is worth reporting; anything else stays installed
     * so a later real fault can still be named. Nothing is ever recovered. */
    if (reported && g_hud_row3_veh) {
        RemoveVectoredExceptionHandler(g_hud_row3_veh);
        g_hud_row3_veh = NULL;
        LOG("hud row3: fault reporter removed; the crash continues as before");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void hud_row3_veh_install(void)
{
    if (g_hud_row3_veh || !g_cfg.hud_row3)
        return;
    g_hud_row3_veh = AddVectoredExceptionHandler(1, hud_row3_veh);
    LOG("hud row3: fault reporter %s", g_hud_row3_veh ? "installed" : "failed");
}

/* One worker tick: keep the third row alive while the mission HUD is up with
 * four players. Never unregisters; stops touching as soon as the mode is off.
 * Placement and showing move to the game thread once the piggyback fires. */
static void hud_row3_tick(void)
{
    if (!g_cfg.hud_row3 || g_cfg.max_players < 4 || g_hud_row3_failed)
        return;
    hud_row3_veh_install();
    const int tick_no = (int)InterlockedIncrement(&g_hud_row3_ticks);
    void **const registry = hud_row3_elements();
    const int natives = registry && hud_row3_native(hud_row3_elem_at(registry, 0x74)) &&
                        hud_row3_native(hud_row3_elem_at(registry, 0x75));
    const int wanted = hud_row_wanted();
    const int mission = hud_row3_mission();
    const int gen = hud_row3_generation();
    /* One line per gate change (and a short heartbeat) so a silent worker is
     * never ambiguous again. */
    const int snapshot = (registry != NULL) | (natives << 1) | (wanted << 2) |
                         (mission << 3);
    if (tick_no <= 6 || snapshot != g_hud_row3_gate_logged) {
        g_hud_row3_gate_logged = snapshot;
        LOG("hud row3: tick %d gate registry=%d natives=%d tokoyo=%d mission=%d "
            "gen=%d element=%d", tick_no, registry != NULL, natives, wanted, mission,
            gen, g_hud_row3_gen);
    }
    if (!natives) {
        g_hud_row3_settle = 0;
        return;
    }
    if (!wanted)
        return;                       /* mode off: park, do not unwind */
    if (!mission)
        return;                       /* prep/lobby HUD: the third row is mission-only */
    /* Rows 1/2 are animated by a per-frame caller reached through the menu's
     * heap (script) objects, so there is no call site to copy. The class's own
     * vtable is the reachable equivalent: patch its two per-frame slots and the
     * game's calls to rows 1/2 become our tick for row 3. */
    hud_row3_native_hook();
    if (InterlockedExchange(&g_hud_row3_dead, 0) && g_hud_row3_elem) {
        /* The game destroyed our element, or a drive found it stale. Only act
         * when there is something to drop: resetting the settle for a no-op is
         * what starved the rebuild in trial 4 (dead never stayed cleared long
         * enough for the two-tick settle to elapse). */
        uint8_t *const slot = (uint8_t *)registry + (size_t)HUD_ROW3_ID * 8;
        int cleared = 0;
        if (readable(slot, sizeof(void *)) && *(void **)slot == g_hud_row3_elem) {
            *(void **)slot = NULL;
            cleared = 1;
        }
        LOG("hud row3: dropped the element from gauge generation %d "
            "(slot %s), rebuilding after the settle", g_hud_row3_gen,
            cleared ? "cleared" : "already replaced");
        g_hud_row3_elem = NULL;
        g_hud_row3_char = NULL;
        g_hud_row3_char_slot = -1;
        g_hud_row3_gen = -1;
        memset(g_hud_row3_block, 0, sizeof g_hud_row3_block);
        g_hud_row3_settle = 0;
    }
    void *const row3_found = hud_row3_elem_at(registry, HUD_ROW3_ID);
    const int ours = row3_found && hud_row3_ours(row3_found);
    void *row3 = ours ? row3_found : NULL;
    if (row3 && g_hud_row3_gen != gen) {
        /* Our element survived the registry but the gauge was rebuilt under it:
         * it and its sprites belong to the previous HUD. Never drive it. */
        if (gen != g_hud_row3_stale_gen) {
            g_hud_row3_stale_gen = gen;
            LOG("hud row3: element belongs to gauge generation %d, now %d; dropping it",
                g_hud_row3_gen, gen);
        }
        uint8_t *const slot = (uint8_t *)registry + (size_t)HUD_ROW3_ID * 8;
        if (readable(slot, sizeof(void *)) && *(void **)slot == row3)
            *(void **)slot = NULL;
        g_hud_row3_elem = NULL;
        g_hud_row3_char = NULL;
        g_hud_row3_char_slot = -1;
        g_hud_row3_gen = -1;
        memset(g_hud_row3_block, 0, sizeof g_hud_row3_block);
        g_hud_row3_settle = 0;
        return;
    }
    if (row3_found && !ours) {
        /* A previous screen's element may still be on its way out, so wait
         * out the same settle the build waits for before calling 0x73 taken. */
        if (g_hud_row3_settle < HUD_ROW3_SETTLE) {
            g_hud_row3_settle++;
            return;
        }
        /* Some other element owns 0x73 in this session's UI. Replacing it
         * would break whatever it belongs to, so row 3 stays off. */
        LOG("hud row3: element slot %#x already holds %p (not ours), "
            "skipping the third row this session", HUD_ROW3_ID, row3_found);
        g_hud_row3_failed = 1;
        return;
    }
    if (!row3) {
        if (g_hud_row3_settle < HUD_ROW3_SETTLE) {
            g_hud_row3_settle++;
            return;
        }
        if (g_hud_row3_attempts >= 3) {
            LOG("hud row3: giving up after %d build attempts", g_hud_row3_attempts);
            g_hud_row3_failed = 1;
            return;
        }
        g_hud_row3_attempts++;
        LOG("hud row3: element slot %#x is free, creating the third row", HUD_ROW3_ID);
        row3 = hud_row3_build(registry);
        if (!row3)
            return;
    }
    const int kept = hud_row3_maintain();
    if (tick_no <= 6)
        LOG("hud row3: tick %d maintain kept=%d (companion slot %d)",
            tick_no, kept, g_hud_row3_char_slot);
    if (!kept) {
        /* Never show a row we cannot feed: an unbound block C would render
         * whatever the empty state does, and a duplicate of row 1 is worse. */
        static int logged;
        if (!logged) {
            logged = 1;
            LOG("hud row3: no companion character distinct from rows 1/2 yet; "
                "row stays hidden");
        }
        return;
    }
    if (g_hud_row3_driven)
        return;                     /* the piggyback now places and shows it */
    /* Fallback until a native row has gone through the patched slots: arm the
     * frame thunk, which runs on the game thread if the game calls it at all. */
    InterlockedExchange(&g_hud_row3_pending, 1);
    if (!g_hud_row3_frame_fired && ++g_hud_row3_starved == 10)
        LOG("hud row3: no game-thread frame call yet; the row stays unplaced");
}

/* The game builds the HUD once per mission but rewrites the state word while
 * it settles, so keep the rows shown for as long as they exist. */
static DWORD WINAPI hud_row_worker(LPVOID unused)
{
    (void)unused;
    for (int tick = 0; tick < 1200; tick++) {
        if (hud_row_wanted()) {
            const LONG n = g_hud_row_count;
            for (LONG i = 0; i < n; i++)
                hud_row_show(g_hud_rows[i]);
        }
        hud_row3_tick();
        Sleep(500);
    }
    /* Clear the latch so a later HUD build starts a fresh watch: the game
     * rebuilds the rows on every mission, and the latched flag used to leave
     * the second and later HUDs with neither a restoration watch nor a row-3
     * drive. */
    LOG("hud row: restoration watch ended, a later HUD build starts a new one");
    InterlockedExchange(&g_hud_row_worker_started, 0);
    return 0;
}

static void *det_HudRow(void *self, int id, int index)
{
    void *const row = g_hud_row_ctor(self, id, index);
    /* Every build of row 0x74 starts a new gauge generation: a new HUD (prep
     * screen, mission entry, mode change) rebuilds rows 0x74/0x75 and the
     * third row we made for the previous one is stale from that moment on. */
    if (g_cfg.hud_row3 && id == 0x74 && row && !index) {
        const LONG gen = InterlockedIncrement(&g_hud_row_gen);
        LOG("game hud: native gauge generation %ld (mission loader %s)",
            gen, hud_row3_mission() ? "in mission" : "prep");
    }
    if (g_cfg.max_players >= 3 && id != 0x74 && row && g_hud_row_count < HUD_ROWS_MAX) {
        const LONG slot = InterlockedIncrement(&g_hud_row_count) - 1;
        if (slot < HUD_ROWS_MAX) {
            g_hud_rows[slot] = row;
            LOG("game hud: companion row id=%#x built", id);
            if (hud_row_wanted())
                hud_row_show(row);
        }
        if (InterlockedCompareExchange(&g_hud_row_worker_started, 1, 0) == 0)
            CreateThread(NULL, 0, hud_row_worker, NULL, 0, NULL);
    }
    return row;
}

static InlineHook g_inline_hooks[] = {
    {HUD_ROW_CTOR_RVA, 19, "hud row", (void *)det_HudRow, (void **)&g_hud_row_ctor,
     {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x48, 0x83, 0xec, 0x30,
      0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff}, relocate_hud_row},
    {0xd6a560, 21, "game send", (void *)det_GameSend, (void **)&g_game_send,
     {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
      0x48, 0x8d, 0xac, 0x24, 0x58, 0xfe, 0xff, 0xff}, NULL},
    {0x8af1b0, 17, "coop wait flag", (void *)det_WaitFlag, (void **)&g_wait_flag,
     {0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05, 0x25, 0x24, 0xfc, 0x00,
      0x8b, 0x88, 0xb0, 0x05, 0x00, 0x00}, relocate_wait_flag},
#ifdef NIOH3P_RESEARCH
    {0xd6acc0, 19, "game dispatch", (void *)det_GameDispatch, (void **)&g_game_dispatch,
     {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x57, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1,
      0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00}, NULL},
#endif
};

static void abs_jmp(uint8_t *at, const void *to)
{
    uintptr_t target = (uintptr_t)to;
    at[0] = 0xff; at[1] = 0x25; at[2] = at[3] = at[4] = at[5] = 0;
    memcpy(at + 6, &target, 8);
}

/* 1 installed, 0 bytes not (yet) the expected plaintext, -1 failed. */
static int inline_install(InlineHook *h)
{
    uint8_t *site = g_base + h->rva;
    if (!readable(site, h->n) || memcmp(site, h->bytes, h->n))
        return 0;
    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    if (!tramp) {
        LOG("game tokoyo: no trampoline for %s (err %lu)", h->name, GetLastError());
        return -1;
    }
    unsigned body = h->relocate ? h->relocate(tramp) : h->n;
    if (!h->relocate)
        memcpy(tramp, site, h->n);
    abs_jmp(tramp + body, site + h->n);
    if (!VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old)) {
        LOG("game tokoyo: cannot seal the %s trampoline (err %lu)", h->name, GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    uint8_t patch[24];
    memset(patch, 0x90, h->n);
    abs_jmp(patch, h->detour);
    /* Take the site writable before publishing the trampoline: when the
     * protection change fails the hook is retried, so *orig must not point at
     * a trampoline for a site that never jumps, and it must not leak. */
    if (!VirtualProtect(site, h->n, PAGE_EXECUTE_READWRITE, &old)) {
        LOG("game tokoyo: cannot write %s (err %lu)", h->name, GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return -1;
    }
    *h->orig = tramp;
    memcpy(site, patch, h->n);
    FlushInstructionCache(GetCurrentProcess(), site, h->n);
    if (!VirtualProtect(site, h->n, PAGE_EXECUTE_READ, &old))
        LOG("game tokoyo: cannot restore execute protection at %#x (err %lu)",
            h->rva, GetLastError());
    return 1;
}

static DWORD WINAPI tokoyo_hooks_worker(LPVOID unused)
{
    (void)unused;
    /* Sized from the table: a new companion hook must fail to compile rather
     * than overflow these. */
    enum { HOOKS_MAX = (int)(sizeof g_inline_hooks / sizeof g_inline_hooks[0]) };
    enum { HOOK_TRIES = 4 };
    const unsigned n = (unsigned)HOOKS_MAX;
    unsigned ok = 0;
    int state[HOOKS_MAX] = {0};     /* 0 pending, 1 installed, -1 failed */
    int tries[HOOKS_MAX] = {0};     /* failed attempts so far */
    /* Stop once nothing can change any more: "bytes are not plaintext yet" (0)
     * keeps polling for the whole window, because VMProtect decrypts lazily,
     * while a failed install (-1) used to count as settled and so gave up on
     * the first allocation or protection hiccup. */
    for (int waited = 0; waited < 600000; waited += 50) {
        unsigned pending = 0;
        for (int i = 0; i < HOOKS_MAX; i++) {
            if (state[i] > 0 || (state[i] < 0 && tries[i] >= HOOK_TRIES))
                continue;
            state[i] = inline_install(&g_inline_hooks[i]);
            if (state[i] > 0) {
                ok++;
            } else if (state[i] < 0 && ++tries[i] >= HOOK_TRIES) {
                LOG("game tokoyo: %s hook failed %d time(s), giving up",
                    g_inline_hooks[i].name, tries[i]);
            } else {
                pending++;
            }
        }
        if (!pending)
            break;
        Sleep(50);
    }
    LOG("game tokoyo: %u/%u companion hook(s) installed", ok, n);
    return 0;
}

/* -------------------------------------------------- game-side friend invite */

/* A stock invite can never join. The GameLobbyJoinRequested callback handler
 * (0x1405ef7e0) stores the lobby id and requests lobby data, but the pump
 * turns the "lobby owner is someone else" state into error 0x80C81010 and
 * latches the error bits that block every later session open. Nothing ever
 * reaches SessionOpen_Start. Two small changes give an invite the same join
 * the Torii Gate search uses:
 *   1. the pump's state-3 branch ("cmp $0x3,%eax; je" at 0x5ef001) is
 *      retargeted to the state-2 path, which only clears the invite state;
 *   2. the callback's Run slot (vtable 0x141586b60, entry 1) is hooked. After
 *      the original Run, the invited lobby id goes to the game's own
 *      join-by-lobby task 0x140d73380(task, ptr_to_lobby_id), which builds
 *      the kind-1 join descriptor and calls SessionOpen_Start. The
 *      descriptor's second field is a pointer to the lobby id; the join
 *      parameter's SetParam (0x1405f1390) dereferences it exactly once.
 *      The task object is the open-request singleton at [manager+0xA8]. It
 *      only exists once the game's session steps have created it; an idle
 *      client has none (both game call sites read it straight and the game's
 *      step 0x140d65740 creates it through factory 0x140d66910). The same
 *      factory runs here first, so an invite works from a fresh boot. */
static uint64_t g_invite_lobby_storage;
static int g_invite_installed;

static void game_invite_join(uint64_t lobby)
{
    static const uint8_t join_prologue[] = {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x40,
    };
    static const uint8_t factory_prologue[] = {
        0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24,
    };
    uint8_t *join = g_base + 0xd73380;
    if (!readable(join, sizeof join_prologue) ||
        memcmp(join, join_prologue, sizeof join_prologue)) {
        LOG("game invite: join task instructions mismatch; invite dropped");
        return;
    }
    void **manager_slot = (void **)(g_base + 0x17900b0);
    if (!readable(manager_slot, sizeof *manager_slot) || !*manager_slot) {
        LOG("game invite: game manager missing; invite dropped");
        return;
    }
    void *manager = *manager_slot;
    if (!readable(manager, 0xb0)) {
        LOG("game invite: manager unreadable; invite dropped");
        return;
    }
    void *task = *(void **)((uint8_t *)manager + 0xa8);
    if (!task) {
        uint8_t *factory = g_base + 0xd66910;
        if (!readable(factory, sizeof factory_prologue) ||
            memcmp(factory, factory_prologue, sizeof factory_prologue)) {
            LOG("game invite: open task factory mismatch; invite dropped");
            return;
        }
        typedef void (*OpenTaskFactory)(void *manager);
        ((OpenTaskFactory)(void *)factory)(manager);
        task = *(void **)((uint8_t *)manager + 0xa8);
        if (!task) {
            LOG("game invite: open task creation failed; invite dropped");
            return;
        }
        LOG("game invite: open task object created");
    }
    g_invite_lobby_storage = lobby;
    typedef int (*JoinByLobby)(void *, uint64_t *);
    int posted = ((JoinByLobby)(void *)join)(task, &g_invite_lobby_storage);
    LOG("game invite: lobby %llu %s", (unsigned long long)lobby,
        posted ? "join posted" : "join refused (an open request may already be running)");
}

static void game_invite_run(void *self, void *param, uint8_t failed, uint64_t call);
static Hook g_game_invite_hook = {
    .name = "CCallback<CSessionP2pSteam,GameLobbyJoinRequested_t>::Run",
    .detour = (void *)(uintptr_t)game_invite_run,
};

static void game_invite_run(void *self, void *param, uint8_t failed, uint64_t call)
{
    typedef void (*RunFn)(void *, void *, uint8_t, uint64_t);
    ((RunFn)g_game_invite_hook.orig)(self, param, failed, call);
    /* GameLobbyJoinRequested_t: m_ulSteamIDLobby is the first field. */
    if (param && readable(param, sizeof(uint64_t)))
        game_invite_join(*(uint64_t *)param);
}

static void install_game_invite_hooks(void)
{
    /* the capacity hook proves the supported game image; without it (harness,
     * unsupported build) the invite patch has nothing to attach to */
    if (!g_game_create_param_hook.nslots)
        return;
    static const uint8_t pump[] = {0x83, 0xf8, 0x03, 0x74, 0x14};
    void **run_slot = (void **)(g_base + 0x1586b68);
    int ready = 0;
    /* VMProtect decrypts .text lazily: poll tightly for the first minute (an
     * invite can only matter once online play starts), then slowly. */
    for (int waited = 0; waited < 600000 && !ready; ) {
        ready = readable(run_slot - 1, 3 * sizeof(void *)) &&
                executable_ptr(run_slot[-1]) &&
                run_slot[-1] == (void *)(g_base + 0x5f0650) &&
                run_slot[0] == (void *)(g_base + 0x5f07d0) &&
                run_slot[1] == (void *)(g_base + 0x5f0640) &&
                readable(g_base + 0x5ef001, sizeof pump) &&
                !memcmp(g_base + 0x5ef001, pump, sizeof pump);
        if (!ready) {
            int step = waited < 60000 ? 10 : 250;
            Sleep((DWORD)step);
            waited += step;
        }
    }
    if (!ready) {
        LOG("game invite: callback vtable or pump instructions mismatch; not installed");
        return;
    }
    if (!add_slot(&g_game_invite_hook, run_slot)) {
        LOG("game invite: callback slot not hooked; not installed");
        return;
    }
    /* Retarget the state-3 jump to the state-2 clear path: the rel8 operand
     * of the je at 0x5ef004 changes from +0x14 (the error block at 0x5ef01a)
     * to -0x10 (0x5eeff6, which only clears the invite state). Same RWX store
     * window as the allowance sites: the page stays executable for the store
     * and the flush, then returns to RX. */
    uint8_t *imm = g_base + 0x5ef005;
    DWORD old;
    if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
        LOG("game invite: cannot write %#x (err %lu)", 0x5ef005, GetLastError());
        return;
    }
    *imm = 0xf0;
    FlushInstructionCache(GetCurrentProcess(), imm, 1);
    if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READ, &old))
        LOG("game invite: cannot restore execute protection at %#x (err %lu)",
            0x5ef005, GetLastError());
    g_invite_installed = 1;
    LOG("game invite: friend invites join directly (callback hooked, error state neutralised)");
}

#ifdef NIOH3P_RESEARCH
/* Rig trigger. The emulator rig cannot deliver GameLobbyJoinRequested_t, so
 * a file named nioh3p.invite.txt in the game directory (the proxy's working
 * directory) holding a decimal lobby id posts the same join. The file is
 * consumed on use, so the run controls when the "invite" arrives: drive the
 * visitor to the world map first, then write the file. Research build only. */
static int g_runcallbacks_ticks;
static int g_fake_invite_seen;

static void det_SteamRunCallbacks(void)
{
    orig_RunCallbacks();
    if (++g_runcallbacks_ticks % 120)   /* about once per two seconds */
        return;
    if (!g_invite_installed || g_fake_invite_seen)
        return;
    HANDLE f = CreateFileA("nioh3p.invite.txt", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    char buf[32];
    DWORD got = 0;
    BOOL ok = ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    if (!ok)
        return;
    uint64_t v = 0;
    for (DWORD i = 0; i < got; i++) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ')
            continue;
        if (buf[i] < '0' || buf[i] > '9')
            return;             /* not a lobby id; leave the file alone */
        v = v * 10 + (uint64_t)(buf[i] - '0');
    }
    if (!v)
        return;
    g_fake_invite_seen = 1;
    MoveFileA("nioh3p.invite.txt", "nioh3p.invite.used.txt");
    LOG("game invite: synthetic invite for lobby %llu", (unsigned long long)v);
    game_invite_join(v);
}
#endif

/* Does the loaded proxy DLL carry an RT_MANIFEST resource? Walk the resource
 * directory root of this module's PE image; the startup log then proves that
 * the embedded manifest actually reached the linked DLL. */
static int manifest_resource_present(HMODULE self)
{
    uint8_t *base = (uint8_t *)self;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS64 *n = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (n->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    DWORD rva = n->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
    if (!rva)
        return 0;
    IMAGE_RESOURCE_DIRECTORY *root = (IMAGE_RESOURCE_DIRECTORY *)(base + rva);
    IMAGE_RESOURCE_DIRECTORY_ENTRY *entry = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(root + 1);
    unsigned count = root->NumberOfNamedEntries + root->NumberOfIdEntries;
    for (unsigned i = 0; i < count; i++) {
        DWORD name = entry[i].Name;   /* Id in the low 16 bits */
        if (!(name & IMAGE_RESOURCE_NAME_IS_STRING) && (name & 0xFFFF) == 24)
            return 1;                 /* RT_MANIFEST */
    }
    return 0;
}

/* ------------------------------------------------------------ main worker */

#ifdef NIOH3P_RESEARCH
static uint8_t *g_dump_disk;
static size_t g_dump_disk_size;

static DWORD WINAPI dump_worker(LPVOID unused)
{
    (void)unused;
    int waited = 0;
    while (g_dump_disk && waited < 60000) {
        if (text_is_decrypted(g_dump_disk, g_dump_disk_size))
            break;
        /* a copy started outside Steam dies on the DRM stub within ~30 ms,
         * so poll tightly for the first two seconds */
        int step = waited < 2000 ? 10 : 200;
        Sleep((DWORD)step);
        waited += step;
    }
    if (g_dump_disk) {
        VirtualFree(g_dump_disk, 0, MEM_RELEASE);
        g_dump_disk = 0;
    }
    LOG("worker: dump trigger after %d ms", waited);
    dump_image("boot");
    if (g_cfg.redump_after_s > 0) {
        Sleep((DWORD)g_cfg.redump_after_s * 1000);
        dump_image("settled");
    }
    /* VMProtect decrypts page by page as the game runs, so a single dump
     * only holds what had executed by then: keep taking snapshots. */
    if (g_cfg.dump_every_s > 0) {
        for (int n = 1; n <= 12; n++) {
            char tag[32];
            fmt(tag, sizeof tag, "t%ds", n * g_cfg.dump_every_s);
            Sleep((DWORD)g_cfg.dump_every_s * 1000);
            dump_image(tag);
        }
    }
    return 0;
}
#endif

static DWORD WINAPI worker(LPVOID unused)
{
    HINSTANCE self = (HINSTANCE)unused;
    /* No file I/O under the loader lock: the config, the log and the banner are
     * opened here, never in DllMain. The critical section comes first, so LOG()
     * is safe from the first line on. */
    loglock_init();
    DWORD dlen = GetModuleFileNameA(self, g_dir, MAX_PATH);
    if (!dlen || dlen >= MAX_PATH) {
        g_dir[0] = '.';         /* unknown module path: stay relative */
        g_dir[1] = 0;
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

#ifdef NIOH3P_RESEARCH
    /* The on-disk copy exists only for the dump trigger: player mode never
     * reads the 27 MB nioh.exe. */
    char exe[MAX_PATH];
    size_t disk_size = 0;
    uint8_t *disk = 0;
    if (g_cfg.dump_image) {
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        disk = read_whole_file(exe, &disk_size);
        LOG("worker: on-disk exe %s (%u bytes) %s", exe, (unsigned)disk_size,
            disk ? "read" : "UNREADABLE");
        if (disk) {
            IMAGE_SECTION_HEADER *s = find_section(".text");
            if (s && s->PointerToRawData + 0x10000 <= disk_size)
                LOG("worker: on-disk .text entropy=%.3f (8.0 == encrypted)",
                    entropy(disk + s->PointerToRawData, 0x10000));
        }
    }
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
            int eos_waiting = eos_unresolved() && !g_eos_polls;  /* module not mapped */
            if (!eos_waiting && (g_steam_ready || tries >= 600))
                break;
            Sleep(5);
        }
        if (eos_unresolved()) {
            char names[400];
            eos_unresolved_names(names, (int)sizeof names);
            LOG("eos: giving up on %d hook(s) (module %s): %s",
                eos_unresolved(), g_eos_polls ? "mapped" : "not mapped", names);
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
        g_dump_disk = disk;
        g_dump_disk_size = disk_size;
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
