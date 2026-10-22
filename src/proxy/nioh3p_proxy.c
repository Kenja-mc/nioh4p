/*
 * nioh3p - Nioh: Complete Edition online research / 3-player-enablement mod
 *
 * Loaded as a dinput8.dll proxy: nioh.exe statically imports dinput8.dll, so the
 * Windows loader maps this DLL before the game's entry point - i.e. before the
 * VMProtect stub unpacks the encrypted .text section.
 *
 * What it does (nothing is faked):
 *   1. Forwards every real dinput8 export to the system dinput8.
 *   2. With dump_image=1: waits for VMProtect to decrypt .text, then dumps the
 *      *runtime* module image. nioh.exe ships with an encrypted .text
 *      (entropy 8.0), so the game's code can neither be analysed nor patched
 *      statically - this dump is the prerequisite for any code-level mod.
 *   3. Hooks the EOS lobby API and raises MaxLobbyMembers / MaxMembers to the
 *      configured player count. EOSSDK-Win64-Shipping.dll and the game's import
 *      table are not encrypted, so this works without knowing the netcode.
 *      Two mechanisms: the documented IAT slot, plus a fallback that scans the
 *      main module for stored copies of the EOS entry point (a rebuilt or
 *      protected import table would defeat the first one).
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
 * Two modes, selected by the `trace` key in nioh3p.ini:
 *   trace=0 (player mode, the default): only the hooks that change behaviour
 *     (lobby capacity, filter drops, force_unlock, the metadata guard) plus
 *     the rare lobby events (create/join/leave) and the numerical search
 *     filter (forwarded unchanged, but its key names go to the log) are
 *     installed. The
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
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* log2 without libm (entropy diagnostics only) */
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
            if (spec == 'p') {
                v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
                spec = 'x';
            } else if (longs >= 2) {
                v = va_arg(ap, unsigned long long);
            } else if (longs == 1) {
                v = va_arg(ap, unsigned long);
            } else {
                v = (unsigned long long)va_arg(ap, unsigned int);
            }
            int neg = 0;
            if ((spec == 'd' || spec == 'i') && (long long)v < 0) {
                neg = 1;
                v = (unsigned long long)(-(long long)v);
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
    int drop_lock_filter;   /* search: do not filter on ktol_*lock_status */
    int force_unlock;       /* host: advertise the session as unlocked */
    int drop_slots_filter;  /* search: do not filter on free slots */
    int trace;              /* 0 = player mode, 1 = full research instrumentation */
} Config;

/* Compiled-in defaults equal the shipped player config. Designated init: a
 * field reorder or addition can never move a value to the wrong key. */
static Config g_cfg = {
    .max_players       = 3,
    .dump_image        = 0,
    .redump_after_s    = 0,
    .dump_every_s      = 0,
    .hook_eos          = 1,
    .drop_lock_filter  = 1,
    .force_unlock      = 0,
    .drop_slots_filter = 1,
    .trace             = 0,
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
    /* 0 turns the clamps off; 2..4 are the only member counts the patches and
     * the game's arrays support. Anything else falls back to 3. */
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
        "dump_every_s=%d drop_lock_filter=%d force_unlock=%d drop_slots_filter=%d trace=%d",
        g_cfg.max_players, g_cfg.dump_image, g_cfg.redump_after_s, g_cfg.hook_eos, g_out_dir,
        g_cfg.dump_every_s, g_cfg.drop_lock_filter, g_cfg.force_unlock, g_cfg.drop_slots_filter,
        g_cfg.trace);
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
    if (g_cfg.trace)
        log_backtrace("EOS_Lobby_CreateLobby");
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

/* EOS_LobbyModification_AddAttributeOptions */
typedef struct {
    int32_t  ApiVersion;
    const AttributeData *Attribute;
    int32_t  Visibility;
} AddAttributeOptions;

static void (EOS_CALL *orig_JoinLobby)(void *, const void *, void *, const void *);
static uint32_t (EOS_CALL *orig_GetMemberCount)(void *, const void *);
static int32_t (EOS_CALL *orig_AddAttribute)(void *, const void *);
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
 * The RTTI class map (analysis/classmap.txt) gives every virtual method's
 * address, so a sidecar table lets each hook say *which* game function called
 * it. Loaded lazily from <dll dir>\nioh3p.sites.txt, one "0xRVA name" per line
 * (tools/sites.py writes it). Nothing here is required for the mod to work. */
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
    unsigned rva = (unsigned)((uintptr_t)ret - (uintptr_t)GetModuleHandleA(NULL));
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

/* Player mode does not resolve call sites at all: no sites_load(), no line. */
#define SITE_NOTE(label) do { \
    if (g_cfg.trace) { \
        char sb_[240]; \
        LOG("site: %s from %s", (label), site_at(__builtin_return_address(0), sb_, sizeof sb_)); \
    } \
} while (0)

/* The matchmaking interface is selected by exact version string and its
 * SteamMatchMaking009 slots are bound explicitly below. */
typedef uint64_t (EOS_CALL *PFN_SteamCreateLobby)(void *, int, int);
typedef uint8_t  (EOS_CALL *PFN_SteamSetLobbyMemberLimit)(void *, uint64_t, int);
typedef void *   (EOS_CALL *PFN_FindOrCreate)(void *, const char *);
typedef int         (EOS_CALL *PFN_SteamSetLobbyData)(void *, uint64_t, const char *, const char *);
typedef const char *(EOS_CALL *PFN_SteamGetLobbyData)(void *, uint64_t, const char *);
typedef int         (EOS_CALL *PFN_SteamGetNumLobbyMembers)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamGetLobbyMemberLimit)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamSendLobbyChatMsg)(void *, uint64_t, const void *, int);
typedef uint64_t    (EOS_CALL *PFN_SteamJoinLobby)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamRequestLobbyData)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamGetLobbyDataCount)(void *, uint64_t);
typedef int         (EOS_CALL *PFN_SteamGetLobbyDataByIndex)(void *, uint64_t, int, char *, int, char *, int);
typedef void        (EOS_CALL *PFN_SteamLeaveLobby)(void *, uint64_t);
/* MSVC returns CSteamID through a caller-owned buffer after `this`, unlike
 * the flat exports that unwrap that buffer into a uint64_t return value. */
typedef uint64_t *  (EOS_CALL *PFN_SteamGetLobbyOwner)(void *, uint64_t *, uint64_t);
typedef uint64_t *  (EOS_CALL *PFN_SteamGetLobbyMemberByIndex)(void *, uint64_t *, uint64_t, int);
typedef void        (EOS_CALL *PFN_SteamRegisterCallback)(void *, int);
typedef void        (EOS_CALL *PFN_SteamRegisterCallResult)(void *, uint64_t);
typedef void        (EOS_CALL *PFN_CallbackRun)(void *, void *, uint8_t, uint64_t);

static PFN_FindOrCreate orig_FindOrCreate;
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

static uint64_t EOS_CALL det_SteamCreateLobby(void *, int, int);
static uint8_t EOS_CALL det_SteamSetLobbyMemberLimit(void *, uint64_t, int);
static int EOS_CALL det_SteamSetLobbyData(void *, uint64_t, const char *, const char *);
static const char *EOS_CALL det_SteamGetLobbyData(void *, uint64_t, const char *);
static int EOS_CALL det_SteamGetNumLobbyMembers(void *, uint64_t);
static int EOS_CALL det_SteamGetLobbyMemberLimit(void *, uint64_t);
static int EOS_CALL det_SteamSendLobbyChatMsg(void *, uint64_t, const void *, int);
static uint64_t EOS_CALL det_SteamJoinLobby(void *, uint64_t);
static int EOS_CALL det_SteamRequestLobbyData(void *, uint64_t);
static int EOS_CALL det_SteamGetLobbyDataCount(void *, uint64_t);
static int EOS_CALL det_SteamGetLobbyDataByIndex(void *, uint64_t, int, char *, int, char *, int);
static void EOS_CALL det_SteamLeaveLobby(void *, uint64_t);
static uint64_t *EOS_CALL det_SteamGetLobbyOwner(void *, uint64_t *, uint64_t);
static uint64_t *EOS_CALL det_SteamGetLobbyMemberByIndex(void *, uint64_t *, uint64_t, int);
static int EOS_CALL det_SteamInviteUserToLobby(void *, uint64_t, uint64_t);
static const char *EOS_CALL det_SteamGetLobbyMemberData(void *, uint64_t, uint64_t, const char *);
static int EOS_CALL det_SteamSetLobbyMemberData(void *, uint64_t, const char *, const char *);
static uint64_t EOS_CALL det_SteamRequestLobbyList(void *);
static void EOS_CALL det_SteamAddStringFilter(void *, const char *, const char *, int);
static void EOS_CALL det_SteamAddNumericalFilter(void *, const char *, int, int);
static void EOS_CALL det_SteamAddNearValueFilter(void *, const char *, int);
static void EOS_CALL det_SteamAddSlotsFilter(void *, int);
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
static void *EOS_CALL det_FindOrCreateInterface(void *, const char *);

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
 * network path never enters the proxy. */
#define STEAM_TRACE_HOOK(name, target, index) \
    { name, (void *)(uintptr_t)target, (index) + 1, 1, 0, 0 }

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

/* How many vtable methods this mode configures, for the startup mode line. */
static int steam_method_count(void)
{
    SteamHook *tbls[] = { g_steam, g_friends, g_networking, g_sockets };
    const int sizes[] = { NSTEAM, NFRIENDS, NNET, NSOCK };
    int n = 0;
    for (int t = 0; t < (int)(sizeof tbls / sizeof tbls[0]); t++)
        for (int i = 0; i < sizes[t]; i++)
            if (tbls[t][i].slot_index && (g_cfg.trace || !tbls[t][i].trace_only))
                n++;
    return n;
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
    if (g_cfg.trace)
        log_backtrace("Steam_ISteamMatchmaking::CreateLobby");
    PFN_SteamCreateLobby o = (PFN_SteamCreateLobby)g_steam[0].orig;
    return o ? o(self, lobby_type, max_members) : 0;
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
    if (g_cfg.trace)
        log_backtrace("Steam_ISteamMatchmaking::SetLobbyMemberLimit");
    PFN_SteamSetLobbyMemberLimit o = (PFN_SteamSetLobbyMemberLimit)g_steam[1].orig;
    return o ? o(self, lobby, max_members) : 0;
}

static void install_game_metadata_guard(void);

/* Lobby *data* is what a joining client filters on: if the host advertises a
 * two-slot session, a third player's client never lists it as joinable. */
static int EOS_CALL det_SteamSetLobbyData(void *self, uint64_t lobby, const char *key, const char *value)
{
    SITE_NOTE("SteamSetLobbyData");
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

static uint64_t EOS_CALL det_SteamJoinLobby(void *self, uint64_t lobby)
{
    SITE_NOTE("SteamJoinLobby");
    LOG("Steam JoinLobby: lobby=%llu", (unsigned long long)lobby);
    PFN_SteamJoinLobby o = (PFN_SteamJoinLobby)g_steam[7].orig;
    uint64_t call = o ? o(self, lobby) : 0;
    LOG("Steam JoinLobby -> call %llu", (unsigned long long)call);
    if (g_cfg.trace)
        log_backtrace("Steam JoinLobby");
    return call;
}

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
    uint64_t *result = o(self, out, lobby);
    LOG("Steam GetLobbyOwner -> %llu", (unsigned long long)*result);
    return result;
}

static uint64_t *EOS_CALL det_SteamGetLobbyMemberByIndex(void *self, uint64_t *out, uint64_t lobby, int index)
{
    SITE_NOTE("SteamGetLobbyMemberByIndex");
    PFN_SteamGetLobbyMemberByIndex o = (PFN_SteamGetLobbyMemberByIndex)g_steam[13].orig;
    uint64_t *result = o(self, out, lobby, index);
    LOG("Steam GetLobbyMemberByIndex[%d] -> %llu", index, (unsigned long long)*result);
    return result;
}

static void EOS_CALL det_SteamLeaveLobby(void *self, uint64_t lobby)
{
    SITE_NOTE("SteamLeaveLobby");
    LOG("Steam LeaveLobby: lobby=%llu", (unsigned long long)lobby);
    PFN_SteamLeaveLobby o = (PFN_SteamLeaveLobby)g_steam[11].orig;
    if (o)
        o(self, lobby);
}

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

static uint64_t EOS_CALL det_SteamRequestLobbyList(void *self)
{
    uint64_t r = g_steam[17].orig ? ((uint64_t EOS_CALL (*)(void *))g_steam[17].orig)(self) : 0;
    SITE_NOTE("RequestLobbyList");
    LOG("Steam RequestLobbyList -> call %llu (results arrive as LobbyMatchList call-result)",
        (unsigned long long)r);
    return r;
}

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

static void EOS_CALL det_SteamAddNearValueFilter(void *self, const char *key, int value)
{
    SITE_NOTE("AddRequestLobbyListNearValueFilter");
    LOG("Steam lobby filter: \"%.60s\" near %d", key ? key : "", value);
    if (g_steam[20].orig)
        ((void EOS_CALL (*)(void *, const char *, int))g_steam[20].orig)(self, key, value);
}

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
    uint64_t *r = ((uint64_t *EOS_CALL (*)(void *, uint64_t *, int))g_steam[25].orig)(self, out, index);
    SITE_NOTE("GetLobbyByIndex");
    LOG("Steam GetLobbyByIndex(%d) -> lobby %llu", index, (unsigned long long)*r);
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
    uint64_t *r = ((uint64_t *EOS_CALL (*)(void *, uint64_t *, int, int))g_friends[FR_BYINDEX].orig)
                 (self, out, index, flags);
    LOG("Steam friends: GetFriendByIndex(%d, flags=0x%x) -> %llu", index, flags, (unsigned long long)*r);
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
static int EOS_CALL det_P2PSend(void *self, uint64_t peer, const void *data, unsigned size, int send_type, int channel)
{
    SITE_NOTE("P2PSend");
    int r = ((int EOS_CALL (*)(void *, uint64_t, const void *, unsigned, int, int))g_networking[P_SEND].orig)
          ? ((int EOS_CALL (*)(void *, uint64_t, const void *, unsigned, int, int))g_networking[P_SEND].orig)(self, peer, data, size, send_type, channel) : 0;
    LOG("Steam p2p: SendP2PPacket(peer=%llu size=%u type=%d chan=%d) -> %d",
        (unsigned long long)peer, size, send_type, channel, r);
    return r;
}

static int EOS_CALL det_P2PAvailable(void *self, unsigned *size, int channel)
{
    SITE_NOTE("P2PAvailable");
    int r = ((int EOS_CALL (*)(void *, unsigned *, int))g_networking[P_AVAIL].orig)
          ? ((int EOS_CALL (*)(void *, unsigned *, int))g_networking[P_AVAIL].orig)(self, size, channel) : 0;
    LOG("Steam p2p: IsP2PPacketAvailable(chan=%d) -> %d size=%u", channel, r, r && size ? *size : 0);
    return r;
}

static int EOS_CALL det_P2PRead(void *self, void *dest, unsigned cap, unsigned *size, uint64_t *from, int channel)
{
    SITE_NOTE("P2PRead");
    int r = ((int EOS_CALL (*)(void *, void *, unsigned, unsigned *, uint64_t *, int))g_networking[P_READ].orig)
          ? ((int EOS_CALL (*)(void *, void *, unsigned, unsigned *, uint64_t *, int))g_networking[P_READ].orig)(self, dest, cap, size, from, channel) : 0;
    LOG("Steam p2p: ReadP2PPacket(chan=%d) -> %d size=%u from=%llu",
        channel, r, r && size ? *size : 0, r && from ? (unsigned long long)*from : 0ull);
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

static void *EOS_CALL det_FindOrCreateInterface(void *user, const char *version)
{
    void *iface = orig_FindOrCreate ? orig_FindOrCreate(user, version) : 0;
    if (g_cfg.trace)
        LOG("Steam FindOrCreateUserInterface(\"%s\") -> %p", version ? version : "(null)", iface);
    if (iface && version) {
        if (!strcmp(version, "SteamMatchMaking009")) {
            steam_apply_table(iface, g_steam, NSTEAM);
            if (g_cfg.trace)
                log_backtrace("Steam matchmaking interface");
        } else if (g_cfg.trace && !strcmp(version, "SteamFriends017")) {
            steam_apply_table(iface, g_friends, NFRIENDS);
            log_backtrace("Steam friends interface");
        } else if (g_cfg.trace && !strcmp(version, "SteamNetworkingSockets009")) {
            steam_apply_table(iface, g_sockets, NSOCK);
        } else if (g_cfg.trace && !strcmp(version, "SteamNetworking006")) {
            steam_apply_table(iface, g_networking, NNET);
        }
    }
    return iface;
}

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
    { "EOS_Lobby_JoinLobby",                 (void *)(uintptr_t)det_JoinLobby,      (void **)&orig_JoinLobby,      1 },
    { "EOS_LobbyDetails_GetMemberCount",     (void *)(uintptr_t)det_GetMemberCount, (void **)&orig_GetMemberCount, 1 },
    { "EOS_LobbySearch_SetParameter",        (void *)(uintptr_t)det_LobbySearchSetParameter,
                                                                                      (void **)&orig_LobbySearchSetParameter, 0 },
    { "EOS_LobbyModification_AddAttribute",  (void *)(uintptr_t)det_AddAttribute,   (void **)&orig_AddAttribute,   0 },
    { "EOS_LobbyDetails_CopyInfo",           (void *)(uintptr_t)det_CopyInfo,       (void **)&orig_CopyInfo,       1 },
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
        if (g_cfg.trace)
            bt_resolve();
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
    /* W^X and AV heuristics: write code through a non-executable RW mapping,
     * flush, then leave the page explicitly RX instead of restoring RWX. */
    if (!VirtualProtect(site, sizeof patch, PAGE_READWRITE, &old)) {
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
 * consumption, departure and close paths are unchanged. The shrine setter
 * 0x140d5fb30 itself only accepts values below 3. Code is plaintext only after
 * the stub has decrypted .text, so wait for the exact original bytes. */
typedef struct {
    uint32_t rva;           /* first byte of the instruction */
    uint8_t len;
    uint8_t imm;            /* offset of the immediate's low byte */
    uint8_t setter;         /* value is an argument to 0x140d5fb30 */
    uint8_t bytes[10];
} AllowanceSite;

static const AllowanceSite g_allowance_sites[] = {
    /* session root reset: movl $1, 0x39c(%rcx) */
    {0xd7ac0c, 10, 6, 0, {0xc7, 0x81, 0x9c, 0x03, 0, 0, 0x01, 0, 0, 0}},
    /* shrine offering opens recruitment: movb $1, 0x39c(%rbx) */
    {0xd7bb77, 7, 6, 0, {0xc6, 0x83, 0x9c, 0x03, 0, 0, 0x01}},
    /* controller reset: movb $1, R+0x39c */
    {0xd7ca03, 7, 6, 0, {0xc6, 0x05, 0x02, 0x44, 0xa1, 0x00, 0x01}},
    /* start recruiting: movb $1, R+0x39c */
    {0xd7daa8, 7, 6, 0, {0xc6, 0x05, 0x5d, 0x33, 0xa1, 0x00, 0x01}},
    /* online shrine offering: mov $1, %ecx; call 0x140d5fb30 */
    {0xcf9f36, 10, 1, 1, {0xb9, 0x01, 0, 0, 0, 0xe8, 0xf0, 0x5b, 0x06, 0x00}},
};

static void install_game_visitor_allowance(void)
{
    if (g_cfg.max_players < 3 || !g_game_create_param_hook.nslots)
        return;
    uint8_t allowance = (uint8_t)(g_cfg.max_players > 4 ? 3 : g_cfg.max_players - 1);
    uint8_t setter_allowance = allowance > 2 ? 2 : allowance;
    const unsigned n = sizeof g_allowance_sites / sizeof g_allowance_sites[0];
    int ready = 0;
    /* VMProtect decrypts .text lazily: poll tightly for the first minute (the
     * mission start is the interesting window), then slowly for nine more. */
    for (int waited = 0; waited < 600000 && !ready; ) {
        ready = 1;
        for (unsigned i = 0; i < n && ready; i++) {
            const AllowanceSite *s = &g_allowance_sites[i];
            ready = readable(g_base + s->rva, s->len) &&
                    !memcmp(g_base + s->rva, s->bytes, s->len);
        }
        if (!ready) {
            int step = waited < 60000 ? 10 : 250;
            Sleep((DWORD)step);
            waited += step;
        }
    }
    if (!ready) {
        LOG("game visitor allowance: recruitment instructions mismatch; not installed");
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        const AllowanceSite *s = &g_allowance_sites[i];
        uint8_t *imm = g_base + s->rva + s->imm;
        DWORD old;
        /* W^X and AV heuristics: RW for the write, then RX again - never RWX. */
        if (!VirtualProtect(imm, 1, PAGE_READWRITE, &old)) {
            LOG("game visitor allowance: cannot write %#x (err %lu)", s->rva, GetLastError());
            continue;
        }
        *imm = s->setter ? setter_allowance : allowance;
        FlushInstructionCache(GetCurrentProcess(), imm, 1);
        if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READ, &old))
            LOG("game visitor allowance: cannot restore execute protection at %#x (err %lu)",
                s->rva, GetLastError());
    }
    LOG("game visitor allowance: recruitment opens %u visitor slot(s) (shrine setter %u)",
        allowance, setter_allowance);
}

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

    if (g_cfg.dump_image) {
        int waited = 0;
        while (disk && waited < 60000) {
            if (text_is_decrypted(disk, disk_size))
                break;
            /* a copy started outside Steam dies on the DRM stub within ~30 ms,
             * so poll tightly for the first two seconds */
            int step = waited < 2000 ? 10 : 200;
            Sleep((DWORD)step);
            waited += step;
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
    }
    /* the allowance poll can wait up to 10 minutes; keep it from delaying dumps */
    if (g_cfg.hook_eos && mod_active)
        install_game_visitor_allowance();
    if (disk)
        VirtualFree(disk, 0, MEM_RELEASE);

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
