/* nioh4p-shrine-respawn: opt-in native shrine respawn for self-recovery,
 * extracted from the combined proxy's "lobby-rule native hooks" and "shared
 * world" sections (src/proxy/nioh3p_proxy.c, 8b52d54).
 *
 * Owned native hooks:
 *   0x949360 Mission::YashiroAccessFunction - checkpoint recording. The native
 *     function already calls 0x8ff270(save, shrine id) for every mode, which
 *     sets the visited-shrine bit and marks the save dirty. The non-Tokoyo
 *     branch additionally stores the id in manager+0x114, but the Tokoyo branch
 *     skips that store. After the native call, and only when the same selected
 *     shrine and its just-set visited bit are readable, the detour mirrors the
 *     id into manager+0x114. manager+0x114 is the field the stock death action
 *     at 0xa84a8b reads before calling 0x87de00(manager, id), so it is the one
 *     checkpoint authority; there is no cross-module last-shrine export or
 *     copied cache. In the private LAN rig it changed 1 -> 0 on native mission
 *     abort and initialized to 1 in the next mission. Actual last-used shrine
 *     recovery across missions remains unproven.
 *   0x884b60 the manager respawn state machine. When a position-based respawn
 *     is pending and the rule is on, the pending point is replaced once per
 *     recovery through the native shrine setter 0x87de00 before the state
 *     machine copies it to the character (state 3).
 *
 * Every path calls the original trampoline, including all early returns. When
 * no usable native point exists - the checkpoint is -1, the setter prologue is
 * not the verified plaintext yet, or the shrine id is missing from the role's
 * gadget tree - the native body position is kept untouched. The once-per-
 * recovery latch is cleared whenever no position-based respawn is pending, so
 * the state-2 update cannot reset the timer every tick.
 *
 * The rule value comes from the core through rules_changed (this module copies
 * only its own field). The local config value applies when this process owns
 * the lobby. Installing this add-on opts in: respawn_at_shrine defaults to 1,
 * and 0 keeps the shipped body-position respawn.
 *
 * This extraction is not evidence that the feature works: shrine respawn is
 * still unproven live and this candidate needs new evidence. Nothing here
 * proves mission freshness or that an actual shrine respawn happened. */
#include "world_helpers.h"

#define N4P_SHRINE_ACCESS_RVA 0x949360u
#define N4P_RESPAWN_STATE_RVA 0x884b60u
#define N4P_SHRINE_SETTER_RVA 0x87de00u
/* Respawn state machine fields inside the online manager. */
#define N4P_MGR_STATE 0x5f0u      /* int state: 0 idle, 1/2 pending, 3 copy */
#define N4P_MGR_POINT 0x614u      /* int pending point id; <0 = body position */

static const Nioh4pCoreApi *g_core;
static int (*g_access_orig)(void *);
static void (*g_respawn_orig)(void *manager);
/* Written only by rules_changed through InterlockedExchange. */
static volatile LONG g_respawn_on;
/* Set once per pending recovery, before calling the native shrine setter. */
static volatile LONG g_respawn_replaced;

/* ------------------------- YashiroAccess checkpoint --------------------- */
/* Mission::YashiroAccessFunction: mov rbp,0x20(rsp); push rdi;
 * sub rsp,0x140; mov cookie,%rax */
static unsigned relocate_shrine_access(uint8_t *out)
{
    static const uint8_t head[] = {0x48, 0x89, 0x6c, 0x24, 0x20, 0x57,
                                   0x48, 0x81, 0xec, 0x40, 0x01, 0x00, 0x00};
    return n4p_world_reloc_cookie(out, head, sizeof head,
                                  g_core ? g_core->game_base : 0);
}

static int det_shrine_access(void *arg)
{
    N4pShrine pre = {0};
    N4pShrine post = {0};
    uint8_t *save;
    uint8_t *m;
    uint32_t bits;
    uint32_t bit;
    const Nioh4pCoreApi *core = g_core;
    const int have_pre = n4p_selected_shrine(core, &pre);
    const int r = g_access_orig(arg);

    /* The native Tokoyo branch skips the checkpoint store. After validated
     * shrine access, mirror the id into the field the native death action
     * consumes. The respawn rule gates only point replacement below. */
    if (!n4p_world_coop(core))
        return r;
    if (!have_pre || !n4p_selected_shrine(core, &post)) {
        core->log("shrine-respawn: shrine access returned without a "
                  "selected shrine");
        return r;
    }
    if (pre.obj != post.obj || pre.id != post.id) {
        core->log("shrine-respawn: shrine changed during access (%u -> %u)",
                  pre.id, post.id);
        return r;
    }
    if (post.id == 0xffffffffu) {
        core->log("shrine-respawn: shrine access had no shrine id");
        return r;
    }
    save = n4p_world_save(core);
    if (!save || !core->readable(save + N4P_SAVE_SHRINE_BITS, 4)) {
        core->log("shrine-respawn: shrine %u save bits unreadable", post.id);
        return r;
    }
    bits = *(const uint32_t *)(save + N4P_SAVE_SHRINE_BITS);
    /* The native setter masks the shift count exactly like 0x8ff270 does. */
    bit = 1u << (post.id & 31u);
    if (!(bits & bit)) {
        core->log("shrine-respawn: shrine %u visited bit %#x absent after "
                  "native (bits %#x)", post.id, bit, bits);
        return r;
    }
    m = n4p_world_manager(core);
    if (!m || !core->readable(m + N4P_MGR_CHECKPOINT, 4)) {
        core->log("shrine-respawn: shrine %u checkpoint field unreadable",
                  post.id);
        return r;
    }
    if (*(const uint32_t *)(m + N4P_MGR_CHECKPOINT) != post.id) {
        *(uint32_t *)(m + N4P_MGR_CHECKPOINT) = post.id;
        core->log("shrine-respawn: shrine %u recorded as checkpoint "
                  "(visited bits %#x)", post.id, bits);
    }
    return r;
}

/* ------------------------- respawn state machine ------------------------ */
/* VMProtect decrypts pages lazily. Only enter the shrine setter when its
 * prologue is the verified plaintext; otherwise the native body-position
 * point is left untouched rather than calling into ciphertext. */
static int shrine_setter_plain(void)
{
    static const uint8_t head[8] = {0x48, 0x8b, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83};
    const uint8_t *fn = (const uint8_t *)g_core->game_base +
                        N4P_SHRINE_SETTER_RVA;
    return g_core->readable(fn, sizeof head) &&
           memcmp(fn, head, sizeof head) == 0;
}

/* 0x87de00's lookup is a map-style tree: head = *(void**)(manager+0x50), and a
 * non-host role also tries *(void**)(manager+0x60) first. Node layout: left
 * +0, parent +8, right +0x10, nil byte +0x19, key u32 +0x20, payload +0x30.
 * A miss makes the setter a no-op, and the state/id it writes are already true
 * before the call, so only this hit is evidence that a replacement happened.
 * Runs in the game thread immediately before the native call. */
static uint8_t *find_shrine(uint8_t *head, int key)
{
    const Nioh4pCoreApi *core = g_core;
    if (!head || !core->readable(head, 0x10))
        return 0;
    uint8_t *node = *(uint8_t *const *)(head + 8);
    uint8_t *found = head;
    for (int steps = 0; steps < 64 && node && core->readable(node, 0x24) &&
         *(const uint8_t *)(node + 0x19) == 0; steps++) {
        if (*(const int32_t *)(node + 0x20) >= key) {
            found = node;
            node = *(uint8_t *const *)node;             /* left */
        } else {
            node = *(uint8_t *const *)(node + 0x10);    /* right */
        }
    }
    if (found == head || *(const int32_t *)(found + 0x20) != key)
        return 0;
    if (!core->readable(found + 0x30, 0x14))
        return 0;
    return found;
}

/* The setter rewrites the pending point and sets the machine back to state 1,
 * so the replacement must happen exactly once per pending respawn: re-firing
 * on the state-2 update would reset the timer to state 1 on every tick and the
 * respawn would never complete. */
static void det_respawn_state(void *manager)
{
    const Nioh4pCoreApi *core = g_core;
    uint8_t *mgr = manager;
    if (mgr && core->readable(mgr, 0x664)) {
        const int state = *(const int32_t *)(mgr + N4P_MGR_STATE);
        const int point_id = *(const int32_t *)(mgr + N4P_MGR_POINT);
        /* States 1..4 are the pending/respawn cycle; state 0 is idle. */
        if (g_respawn_replaced &&
            (state == 0 || state >= 3 || point_id >= 0))
            InterlockedExchange(&g_respawn_replaced, 0);
        if (g_respawn_on && core->max_players >= 3 &&
            *(const uint8_t *)(mgr + N4P_MGR_TOKOYO)) {
            const int role = *(const int32_t *)(mgr + N4P_MGR_ROLE);
            /* State 1/2 is a pending respawn; a negative point id means the
             * point came from a position (the native self-recovery body
             * position), not from a shrine. Do not touch the checkpoint or
             * query the shrine tree otherwise. */
            if ((role == 0 || role == 1) && (state == 1 || state == 2) &&
                point_id < 0) {
                /* Native reset flows write 0, which is also a representable
                 * shrine id. The role-ordered tree hit below establishes
                 * validity; do not reject id 0 as an empty sentinel. */
                const int shrine = *(const int32_t *)(mgr + N4P_MGR_CHECKPOINT);
                const int valid = shrine != -1;
                const int plain = valid ? shrine_setter_plain() : 0;
                /* Mirror the setter's own lookup: role 0 searches
                 * manager+0x50; every other role tries manager+0x60 and then
                 * falls back to +0x50. Only a hit makes it write. */
                uint8_t *node = 0;
                if (valid && plain) {
                    if (role != 0 &&
                        core->readable(mgr + 0x60, sizeof(void *)))
                        node = find_shrine(*(uint8_t *const *)(mgr + 0x60),
                                           shrine);
                    if (!node && core->readable(mgr + 0x50, sizeof(void *)))
                        node = find_shrine(*(uint8_t *const *)(mgr + 0x50),
                                           shrine);
                }
                static volatile LONG no_shrine_logged, cipher_logged;
                static volatile LONG miss_logged;
                if (!valid && InterlockedIncrement(&no_shrine_logged) == 1)
                    core->log("shrine-respawn: self-recovery pending but no "
                              "used shrine; body position kept");
                else if (valid && !plain &&
                         InterlockedIncrement(&cipher_logged) == 1)
                    core->log("shrine-respawn: shrine setter 0x87de00 is not "
                              "plaintext yet; body position kept");
                else if (valid && plain && !node &&
                         InterlockedIncrement(&miss_logged) == 1)
                    core->log("shrine-respawn: shrine %d is not in the role "
                              "tree; body position kept", shrine);
                if (valid && plain && node &&
                    InterlockedCompareExchange(&g_respawn_replaced, 1, 0) == 0) {
                    typedef void (*SetShrinePoint)(void *, unsigned);
                    SetShrinePoint set = (SetShrinePoint)(void *)(g_core->game_base +
                                                                 N4P_SHRINE_SETTER_RVA);
                    set(manager, (unsigned)shrine);
                    /* The setter writes position +0x600, angle +0x610, state 1
                     * and id -1 whenever its own lookup hits, which the scan
                     * above just proved. The pre-existing state/id values are
                     * not used as proof, and neither is this log: nothing here
                     * proves the respawn completed. */
                    core->log("shrine-respawn: self-recovery respawn point -> "
                              "shrine %d (manager+0x114, node found, native "
                              "0x87de00)", shrine);
                }
            }
        }
    }
    if (g_respawn_orig)
        g_respawn_orig(manager);
}

static Nioh4pHook g_respawn_hooks[] = {
    {N4P_SHRINE_ACCESS_RVA, 20, "shrine access", (void *)det_shrine_access,
     (void **)&g_access_orig,
     {0x48, 0x89, 0x6c, 0x24, 0x20, 0x57, 0x48, 0x81, 0xec, 0x40, 0x01,
      0x00, 0x00, 0x48, 0x8b, 0x05, 0x24, 0xec, 0xea, 0x00},
     relocate_shrine_access},
    {N4P_RESPAWN_STATE_RVA, 20, "respawn state", (void *)det_respawn_state,
     (void **)&g_respawn_orig,
     {0x40, 0x57, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00, 0x8b,
      0x81, 0xf0, 0x05, 0x00, 0x00, 0x48, 0x8b, 0xf9, 0x85, 0xc0}, NULL},
};

static void rules_changed(const Nioh4pRules *rules)
{
    /* Only this module's field; the struct is borrowed and may change after
     * the call returns. */
    int on = rules ? rules->respawn_shrine : 0;
    InterlockedExchange(&g_respawn_on, on ? 1 : 0);
}

static Nioh4pAddon g_respawn_addon = {
    NIOH4P_ADDON_ABI, sizeof g_respawn_addon, NIOH4P_SHRINE_RESPAWN,
    0 /* set in Init from config */, g_respawn_hooks,
    sizeof g_respawn_hooks / sizeof g_respawn_hooks[0],
    0 /* set in Init from config */, NULL, rules_changed
};

const Nioh4pAddon *__cdecl Nioh4pAddonInit(const Nioh4pCoreApi *core,
                                           HMODULE module)
{
    int enabled = 1, shrine = 1;
    if (!core || core->version != NIOH4P_ADDON_ABI ||
        core->size < sizeof *core)
        return NULL;
    if (!n4p_config_int(core, module, "enabled", "1", 0, 1, &enabled))
        return NULL;
    if (!n4p_config_int(core, module, "respawn_at_shrine", "1", 0, 1, &shrine))
        return NULL;
    g_core = core;
    g_respawn_addon.enabled = (uint32_t)enabled;
    g_respawn_addon.local_value = shrine;
    return &g_respawn_addon;
}
