/* nioh4p-enemy-health: enemy maximum-HP multiplier extracted from the combined
 * proxy's "lobby-rule native hooks" section (src/proxy/nioh3p_proxy.c, 8b52d54).
 *
 * Owned native hook:
 *   0x7c7eb0 character status recalc: computes max HP from the character data
 *     and the mission scaling, then clamps current HP. The detour scales only
 *     the freshly computed maximum, so the factor cannot compound. It saves
 *     wounded current HP before the native clamp and caps it at the scaled
 *     maximum. Full-health initialization and the existing exclusions remain.
 *
 * The effective percentage is not read from game memory or from another DLL:
 * the core owns lobby authority and calls rules_changed with the effective
 * rules. This module copies only its own field (enemy_hp_pct) into one atomic
 * cache. Config supplies the local value used when this process owns the
 * lobby; the core's stock default is 100.
 *
 * This extraction is not evidence that the feature works: it needs new live
 * evidence. Enemies whose last recalculation ran before the hook was installed
 * keep stock HP for that mission. */
#include "runtime.h"

#define N4P_STATUS_RECALC_RVA 0x7c7eb0u

/* Player data flag observed on all four native party characters. It does not
 * identify friendly escorts: Ginchiyo has this bit clear. */
#define N4P_CHAR_DATA 0xe90u
#define N4P_CHAR_PLAYER_BIT 1u
#define N4P_CHAR_SIDE 0xa8u
#define N4P_SIDE_VALUE 0x10u

/* Assist block at manager+0x660 owns the local character at +0x58 and the one
 * companion at +0xa8, stride 0x50. Those two contexts alone do not cover a
 * four-player session, so the native party slot array is checked as well. */
#define N4P_MGR_ASSIST 0x660u
#define N4P_MGR_ASSIST_CTX 0x58u
#define N4P_MGR_ASSIST_STRIDE 0x50u
#define N4P_MGR_ASSIST_COUNT 2u

static const Nioh4pCoreApi *g_core;
static void (*g_recalc_orig)(void *status);
/* Written only by rules_changed through InterlockedExchange. */
static volatile LONG g_enemy_pct = 100;
/* Plain counters: the recalc hook runs on the game thread only, and the
 * values only set the log cadence. */
static int g_unscaled_seen;
static int g_enemy_seen;

static int character_is_player(const uint8_t *ch)
{
    if (!ch || !g_core->readable(ch + N4P_CHAR_DATA, sizeof(void *)))
        return 0;
    const uint8_t *data = *(const uint8_t *const *)(ch + N4P_CHAR_DATA);
    if (!data || !g_core->readable(data + 4, 4))
        return 0;
    return (*(const uint32_t *)(data + 4) >> N4P_CHAR_PLAYER_BIT) & 1u;
}

static int character_is_party(const uint8_t *ch)
{
    for (unsigned i = 0; i < N4P_PARTY_COUNT; i++) {
        const uint8_t *slot = (const uint8_t *)(g_core->game_base +
            N4P_PARTY_RVA) + (size_t)i * N4P_PARTY_STRIDE;
        if (g_core->readable(slot, sizeof(void *)) &&
            *(const void *const *)slot == (const void *)ch)
            return 1;
    }
    const uint8_t *mgr = n4p_manager(g_core);
    if (!mgr || !g_core->readable(mgr + N4P_MGR_ASSIST,
                                  N4P_MGR_ASSIST_CTX +
                                  N4P_MGR_ASSIST_STRIDE * N4P_MGR_ASSIST_COUNT))
        return 0;
    for (unsigned i = 0; i < N4P_MGR_ASSIST_COUNT; i++) {
        const uint8_t *ctx = mgr + N4P_MGR_ASSIST + N4P_MGR_ASSIST_CTX +
                              (size_t)i * N4P_MGR_ASSIST_STRIDE;
        if (*(const void *const *)ctx == (const void *)ch)
            return 1;
    }
    return 0;
}
/* Character::SetSide (0x893880 -> 0x963400) writes component+0x10.
 * Shot::IsPlayerSide (0x9376e0) compares that field with local party slot0.
 * Live Ginchiyo and the local player share side0; two hostile actors use1.
 * Unknown/partially initialized sides stay native rather than risking allies. */
static int character_side(const uint8_t *ch, uint32_t *side)
{
    if (!ch || !g_core->readable(ch + N4P_CHAR_SIDE, sizeof(void *)))
        return 0;
    const uint8_t *component =
        *(const uint8_t *const *)(ch + N4P_CHAR_SIDE);
    if (!component ||
        !g_core->readable(component + N4P_SIDE_VALUE, sizeof *side))
        return 0;
    *side = *(const uint32_t *)(component + N4P_SIDE_VALUE);
    return 1;
}

static int character_is_opposing_side(const uint8_t *ch)
{
    const uint8_t *const *local = (const uint8_t *const *)
        (g_core->game_base + N4P_PARTY_RVA);
    uint32_t actor_side, local_side;
    return g_core->readable(local, sizeof *local) &&
           character_side(ch, &actor_side) &&
           character_side(*local, &local_side) && actor_side != local_side;
}


static void det_status_recalc(void *status)
{
    uint8_t *st = status;
    const int pct = (int)g_enemy_pct;
    uint64_t prior_cur = 0;
    int was_wounded = 0;
    const int snapshot = pct != 100 && st && g_core->readable(st, 0x28);
    if (snapshot) {
        /* Native recalc clamps against its unscaled maximum. Capture the
         * wound first: that clamp can otherwise turn it into a full value. */
        prior_cur = *(const uint64_t *)(st + 0x20);
        was_wounded = prior_cur < *(const uint64_t *)(st + 0x18);
    }
    if (g_recalc_orig)
        g_recalc_orig(status);
    if (!snapshot || !g_core->readable(st, 0x28))
        return;
    uint8_t *ch = *(uint8_t *const *)st;
    if (!ch)
        return;
    if (character_is_player(ch) || character_is_party(ch) ||
        !character_is_opposing_side(ch)) {
        if (++g_unscaled_seen <= 4)
            g_core->log("enemy-health: actor HP left at %llu/%llu "
                        "(player, party, friendly or unknown side)",
                        (unsigned long long)*(const uint64_t *)(st + 0x18),
                        (unsigned long long)*(const uint64_t *)(st + 0x20));
        return;
    }
    uint64_t *maxp = (uint64_t *)(st + 0x18);
    uint64_t *curp = (uint64_t *)(st + 0x20);
    const uint64_t base = *maxp;
    const uint64_t cur = *curp;
    if (!base)
        return;
    uint64_t scaled = (base * (uint64_t)pct + 50) / 100;
    if (!scaled)
        scaled = 1;
    if (scaled == base)
        return;
    *maxp = scaled;
    if (was_wounded)
        *curp = prior_cur < scaled ? prior_cur : scaled;
    else if (cur == base || cur > scaled)
        *curp = scaled;
    const int n = ++g_enemy_seen;
    if (n <= 8 || n % 500 == 0)
        g_core->log("enemy-health: enemy HP %llu -> %llu (%d%%, current "
                    "%llu -> %llu)",
                    (unsigned long long)base, (unsigned long long)scaled, pct,
                    (unsigned long long)prior_cur, (unsigned long long)*curp);
}

/* Position independent prologue: push rbp; push r12; push r13; push r14;
 * push r15; sub rsp,0x120. No RIP-relative instruction, so the core may replay
 * the 19 bytes as copied. */
static Nioh4pHook g_enemy_hooks[] = {
    {N4P_STATUS_RECALC_RVA, 19, "status recalc", (void *)det_status_recalc,
     (void **)&g_recalc_orig,
     {0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
      0x41, 0x57, 0x48, 0x81, 0xec, 0x20, 0x01, 0x00, 0x00}, NULL},
};

static void rules_changed(const Nioh4pRules *rules)
{
    /* Only this module's field; the struct is borrowed and may change after
     * the call returns. */
    int pct = rules ? rules->enemy_hp_pct : 100;
    if (pct < 25 || pct > 400)
        pct = 100;
    InterlockedExchange(&g_enemy_pct, pct);
}

static Nioh4pAddon g_enemy_addon = {
    NIOH4P_ADDON_ABI, sizeof g_enemy_addon, NIOH4P_ENEMY_HEALTH,
    0 /* set in Init from config */, g_enemy_hooks,
    sizeof g_enemy_hooks / sizeof g_enemy_hooks[0],
    0 /* set in Init from config */, NULL, rules_changed
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
    if (!n4p_config_int(core, module, "enemy_hp_percent", "100", 25, 400,
                        &percent))
        return NULL;
    g_core = core;
    g_enemy_addon.enabled = (uint32_t)enabled;
    g_enemy_addon.local_value = percent;
    return &g_enemy_addon;
}
