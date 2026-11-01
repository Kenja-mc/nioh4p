#include "nioh3p_internal.h"

/* ------------------------------------------------------------------ config */

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

/* Compiled-in defaults equal the shipped player config. The search filters are
 * kept by default, so an unmodified game has exactly the stock discovery rules;
 * the two drops are opt-in workarounds only. Designated init: a field reorder
 * or addition can never move a value to the wrong key. */
Config g_cfg = {
    .max_players       = 4,
    .dump_image        = 0,
    .redump_after_s    = 0,
    .dump_every_s      = 0,
    .hook_eos          = 1,
    .drop_lock_filter  = 0,
    .force_unlock      = 0,
    .drop_slots_filter = 0,
    .trace             = 0,
    .hud_row3          = 1,
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

/* Strict decimal for the lobby-rule keys: sign and digits only, no suffix and
 * no overflow. The other keys keep their historical atoi behaviour. */
int parse_int_strict(const char *s, int *out)
{
    const char *p = s;
    int neg = 0, digits = 0;
    long v = 0;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        p++;
    }
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 9)
            return 0;
        v = v * 10 + (*p - '0');
        p++;
    }
    if (*p)
        return 0;
    if (neg)
        v = -v;
    if (v < -2147483647L - 1 || v > 2147483647L)
        return 0;
    *out = (int)v;
    return 1;
}


void cfg_load(const char *dir)
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
                char *ve = v + strlen(v);
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) *--ve = 0;
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
        g_cfg.max_players = 4;
    }
    g_cfg_status = CFG_LOADED;
}

/* Status line(s) for cfg_load's outcome, then the effective config. */
void cfg_report(void)
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
        LOG("config: max_players=%d out of range; using 4", g_cfg_max_players_raw);
    LOG("config: max_players=%u dump_image=%d redump_after_s=%d hook_eos=%d out_dir=%s "
        "dump_every_s=%d drop_lock_filter=%d force_unlock=%d drop_slots_filter=%d trace=%d "
        "hud_row3=%d",
        g_cfg.max_players, g_cfg.dump_image, g_cfg.redump_after_s, g_cfg.hook_eos, g_out_dir,
        g_cfg.dump_every_s, g_cfg.drop_lock_filter, g_cfg.force_unlock, g_cfg.drop_slots_filter,
        g_cfg.trace, g_cfg.hud_row3);
}
