/* nioh4p-assist-gauge: continuous assist-gauge drain rate extracted from the
 * combined proxy's "lobby-rule native hooks" section (src/proxy/nioh3p_proxy.c,
 * 8b52d54).
 *
 * Owned native patch (no hook):
 *   0x8c1bc5 the inline continuous assist drain (stock 166 per update). Only
 *     the five-byte comparison changes: a rel32 branch replaces the stock
 *     compare inside the aligned 0x8c1bc0 window, which the core publishes
 *     atomically through publish_code16. The old, unaligned 0x8c1bbe memcpy is
 *     not used. Gates before the site and the revive/recovery code after
 *     0x8c1bf6 stay native.
 *
 * The stub is emitted into memory allocated by the core's alloc_rel32, so the
 * jump target is reachable with a near relative branch. It reads this module's
 * own atomic step cache each update and reproduces the stock clamp. The step
 * is the rounded native fraction of 166, cached by rules_changed; the core
 * owns lobby authority and supplies the effective percentage.
 *
 * Only RAX and RCX are clobbered, exactly like the original block; both are
 * dead at the resume point. Stock leaves both gauges alone when the current
 * value is already zero (0x8c1bc3 jumps straight to 0x8c1bf6), so the pending
 * gauge must not be drained alone: that would eat the revive reserve after a
 * gauge-empty failure.
 *
 * This extraction is not evidence that the feature works: it needs new live
 * evidence. */
#include "runtime.h"

#define N4P_ASSIST_WINDOW_RVA 0x8c1bc0u  /* 16-byte aligned publication window */
#define N4P_ASSIST_SITE_OFF 5u           /* the five-byte cmp at 0x8c1bc5 */
#define N4P_ASSIST_RESUME_RVA 0x8c1bf6u

static const Nioh4pCoreApi *g_core;
/* Written only by rules_changed through InterlockedExchange; the machine stub
 * loads it each update. */
static volatile LONG g_assist_drain_step = 166;

static uint8_t *emit8(uint8_t *p, uint8_t v) { *p = v; return p + 1; }
static uint8_t *emit32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); return p + 4; }
static uint8_t *emit64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); return p + 8; }

/* Executable stub for the drain block. It reads g_assist_drain_step each
 * update and reproduces the stock clamp, then jumps to 0x8c1bf6. The emitted
 * instruction sequence is unchanged from the combined proxy. */
static uint8_t *build_assist_stub(uint8_t *p, void *resume)
{
    p = emit8(p, 0x48); p = emit8(p, 0xb8);            /* movabs rax, &step */
    p = emit64(p, (uint64_t)(uintptr_t)&g_assist_drain_step);
    p = emit8(p, 0x8b); p = emit8(p, 0x00);            /* mov eax,[rax] */
    p = emit8(p, 0x85); p = emit8(p, 0xc0);            /* test eax,eax */
    p = emit8(p, 0x0f); p = emit8(p, 0x84);
    uint8_t *done32 = p; p = emit32(p, 0);

    p = emit8(p, 0x8b); p = emit8(p, 0x4e); p = emit8(p, 0x0c);  /* mov ecx,[rsi+0xc] */
    p = emit8(p, 0x85); p = emit8(p, 0xc9);                      /* test ecx,ecx */
    p = emit8(p, 0x7e); uint8_t *pending1 = p; p++;
    p = emit8(p, 0x3b); p = emit8(p, 0xc8);                      /* cmp ecx,eax */
    p = emit8(p, 0x76); uint8_t *zcur8 = p; p++;
    p = emit8(p, 0x2b); p = emit8(p, 0xc8);                      /* sub ecx,eax */
    p = emit8(p, 0x89); p = emit8(p, 0x4e); p = emit8(p, 0x0c);
    p = emit8(p, 0xeb); uint8_t *pending2 = p; p++;
    uint8_t *zcur = p;
    p = emit8(p, 0xc7); p = emit8(p, 0x46); p = emit8(p, 0x0c); p = emit32(p, 0);
    uint8_t *pending = p;

    p = emit8(p, 0x8b); p = emit8(p, 0x4e); p = emit8(p, 0x14);  /* mov ecx,[rsi+0x14] */
    p = emit8(p, 0x85); p = emit8(p, 0xc9);
    p = emit8(p, 0x7e); uint8_t *done8 = p; p++;
    p = emit8(p, 0x3b); p = emit8(p, 0xc8);
    p = emit8(p, 0x76); uint8_t *zpend8 = p; p++;
    p = emit8(p, 0x2b); p = emit8(p, 0xc8);
    p = emit8(p, 0x89); p = emit8(p, 0x4e); p = emit8(p, 0x14);
    p = emit8(p, 0xeb); uint8_t *done3 = p; p++;
    uint8_t *zpend = p;
    p = emit8(p, 0xc7); p = emit8(p, 0x46); p = emit8(p, 0x14); p = emit32(p, 0);
    uint8_t *done = p;

    *pending1 = (uint8_t)(done - (pending1 + 1));
    *pending2 = (uint8_t)(pending - (pending2 + 1));
    *zcur8 = (uint8_t)(zcur - (zcur8 + 1));
    *done8 = (uint8_t)(done - (done8 + 1));
    *zpend8 = (uint8_t)(zpend - (zpend8 + 1));
    *done3 = (uint8_t)(done - (done3 + 1));
    int32_t rel = (int32_t)(done - (done32 + 4));
    memcpy(done32, &rel, 4);

    p = emit8(p, 0xff); p = emit8(p, 0x25); p = emit8(p, 0); p = emit8(p, 0);
    p = emit8(p, 0); p = emit8(p, 0);
    p = emit64(p, (uint64_t)(uintptr_t)resume);
    return p;   /* one past the stub: install uses (end - mem) for flushing */
}

/* Only the five-byte cmp at 0x8c1bc5 changes. Its enclosing aligned window is
 * published atomically; threads past the cmp still see intact stock code.
 * Returns 1 installed, 0 not plaintext yet, -1 failed. Only success is
 * cached. */
static int install_assist_drain(void)
{
    static int state;
    static const uint8_t expected[16] = {
        0x0c, 0x85, 0xc0, 0x7e, 0x31, 0x3d, 0xa6, 0x00,
        0x00, 0x00, 0x7e, 0x0a, 0x05, 0x5a, 0xff, 0xff,
    };
    if (state)
        return state;
    uint8_t *window = (uint8_t *)g_core->game_base + N4P_ASSIST_WINDOW_RVA;
    if (!g_core->readable(window, sizeof expected) ||
        memcmp(window, expected, sizeof expected))
        return 0;
    uint8_t *site = window + N4P_ASSIST_SITE_OFF;
    uint8_t *mem = g_core->alloc_rel32(site, 128);
    if (!mem) {
        g_core->log("assist-gauge: no rel32 stub memory near 0x8c1bc5");
        return -1;
    }
    uint8_t *end = build_assist_stub(mem,
        (uint8_t *)g_core->game_base + N4P_ASSIST_RESUME_RVA);
    DWORD old;
    if (!VirtualProtect(mem, 128, PAGE_EXECUTE_READ, &old)) {
        g_core->log("assist-gauge: cannot seal the drain stub (err %lu)",
                    GetLastError());
        VirtualFree(mem, 0, MEM_RELEASE);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, (SIZE_T)(end - mem));
    uint8_t patch[16];
    memcpy(patch, expected, sizeof patch);
    patch[N4P_ASSIST_SITE_OFF] = 0xe9;
    int32_t relative = (int32_t)((int64_t)(uintptr_t)mem -
                                (int64_t)(uintptr_t)(site + 5));
    memcpy(patch + N4P_ASSIST_SITE_OFF + 1, &relative, sizeof relative);
    if (!g_core->publish_code16(window, expected, patch, "assist drain")) {
        g_core->log("assist-gauge: 0x8c1bc0 window changed during install");
        VirtualFree(mem, 0, MEM_RELEASE);
        return -1;
    }
    state = 1;
    g_core->log("assist-gauge: drain intercepted at 0x8c1bc5 (stock step 166, "
                "atomic rel32)");
    return 1;
}

static void rules_changed(const Nioh4pRules *rules)
{
    int pct = rules ? rules->assist_drain_pct : 100;
    if (pct < 0 || pct > 1000)
        pct = 100;
    int step = (int)(((long)pct * 166 + 50) / 100);
    if (step < 0)
        step = 0;
    InterlockedExchange(&g_assist_drain_step, step);
}

static Nioh4pAddon g_assist_addon = {
    NIOH4P_ADDON_ABI, sizeof g_assist_addon, NIOH4P_ASSIST_GAUGE,
    0 /* set in Init from config */, NULL, 0,
    0 /* set in Init from config */, install_assist_drain, rules_changed
};

const Nioh4pAddon *__cdecl Nioh4pAddonInit(const Nioh4pCoreApi *core,
                                           HMODULE module)
{
    int enabled = 1, percent = 100;
    if (!core || core->version != NIOH4P_ADDON_ABI ||
        core->size < sizeof *core)
        return NULL;
    if (!n4p_config_int(core, module, "enabled", "1", 0, 1, &enabled))
        return NULL;
    if (!n4p_config_int(core, module, "assist_drain_percent", "100", 0, 1000,
                        &percent))
        return NULL;
    g_core = core;
    g_assist_addon.enabled = (uint32_t)enabled;
    g_assist_addon.local_value = percent;
    return &g_assist_addon;
}
