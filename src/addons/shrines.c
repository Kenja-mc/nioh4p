/* nioh4p-shrines: extracted Mission::IsYashiroActive co-op gate lift plus
 * research-only shrine diagnostics. Extracted from the combined proxy's
 * "shared world" section; see src/addons/world_helpers.h for the field
 * offsets and docs/RE_TOKOYO_COOP.md for the reverse-engineering notes.
 *
 * Owned native hooks:
 *   0x9488a0 Mission::IsYashiroActive - the co-op gate lift below.
 *   0x949620 Mission::YashiroOpenMenu - RESEARCH build only: event-only
 *     observation; a player build hooks nothing here. The native body does
 *     not open anything itself: it closes existing UI windows and stores the
 *     selected shrine gadget into the manager+0xa90 weak-ref slot; the menu
 *     itself opens through the task dispatch 0x140bf55c0 in the same body.
 *   0x949090 Mission::YashiroAccessBonusAmurita (name confirmed against the
 *     script-binding table at image 0x120eaa0) - RESEARCH build only:
 *     observation of the raw native return word. It is an action binding, not
 *     a predicate: the traced body never calls the completion glue
 *     0x140ddfd00, so it pushes no Lua boolean at all.
 * Both features are script bindings, so the game's own mission script decides
 * when they run. The gate hook only changes the native decision where the
 * stock two-player assumptions lock the other players out; a real 3/4-player
 * party is required, so a solo or stock two-player mission keeps every native
 * gate.
 *
 * Mission::IsYashiroActive native semantics (0x1409488a0, verified in
 * text.dis): the result starts as manager+0x629; a valid selected shrine
 * (kind 2, richdata present) ANDs it with rich+0x1a82 != 0; manager+0x626
 * != 0 forces 0; and only when manager+0x660 (Tokoyo) is set does
 * manager+0x662 != 0 force 0. The answer byte is pushed into the script
 * frame by the completion glue 0x140ddfd00 at 0x140948941; the function's
 * own return word is just the movzwl of that frame's +0x10c counter, never
 * the boolean.
 *
 * Captured 4P Tokoyo evidence (ffec-pre-modular-baseline, all four slots):
 * 0x626 = 0, 0x629 = 1, 0x660 = 1, 0x662 = 0 with access 1 on the polled
 * shrines, so the native computed TRUE with nothing for the gate lift to
 * clear - and the host still showed no Pray prompt (shots
 * combined-ffec-first-shrine-pray.png / combined-ffec-shrine-interact-l.png).
 * The gate-lift log fired ~30.9k times per client for shrines 2 and 3 over
 * ~20 minutes (a per-frame poll) but exactly once for shrine 1, and the
 * research window (first 64 changes) likewise shows one shrine-1 call while
 * 2/3 were polled ~31 times each, so the prompt is not driven by this
 * binding either. The prompt path lives in the native interaction-area
 * pipeline (offsets recorded by the research observation below): gadget+0x98
 * area container with entries at +0x20+i*0x120 and the active byte at
 * entry+0x8 (Interaction::ActivateArea 0x1409443b0 / DeactivateArea
 * 0x140944430), the bbox-valid byte at container+0x4c0 written by the
 * per-frame area update 0x140774890, the shrine containment scan 0x140861d00
 * (kind 2, container+0x4c0, bbox, then point-in-area via 0x140951bd0), and
 * the interaction target stored at [[char+0x230]+8]+0x510 by the interaction
 * event dispatch 0x14073ffb0.
 *
 * [INFERENCE] 0x662 is the "shrine used" latch behind
 * Gadget::SetYashiroOnlineDoneAccess (the mission event handler at
 * 0x1407bf723 sets it on event id 0x4545), and text.dis contains no write
 * that clears it (seven references: five reads, that one setter), so without
 * help the prompt would never return in Tokoyo once set. The native body
 * reads 0x662 only when manager+0x660 is set. The binding writes its answer
 * into the script argument (0x140ddfd00 at 0x140948941), so returning 1 from
 * this detour cannot change what the mission script sees: the hook clears
 * the two co-op latches only for the duration of the native body (0x662 only
 * in Tokoyo) and restores them immediately. Between the latch reads at
 * 0x140948914/0x14094892f the native body runs only the binding glue
 * 0x140ddf4e0/0x140ddfd00, neither of which references the latches; every
 * other reader and writer of 0x626/0x662 in the binary is game-thread
 * mission/script code.
 */
#include "world_helpers.h"

static const Nioh4pCoreApi *g_core;

/* ---------------- RESEARCH-ONLY shrine-state observation ---------------- */
#ifdef NIOH3P_RESEARCH
/* RESEARCH-ONLY observation of the shrine bindings' surroundings.
 * Purely diagnostic: it reads game memory behind readable() checks, never
 * writes, and logs one line only when the observation tuple changes, so a
 * per-frame prompt check cannot flood the log; a hard cap silences even an
 * oscillating tuple. `native_return` is the raw word the native function
 * returned in eax. text.dis does NOT establish that this word is the Lua
 * boolean: the completion glue 0x140ddfd00 pushes the answer byte into the
 * VM through 0x1404603b0 and merely does `incl 0x10c(%frame)`, and the
 * function's return value is the movzwl of that same frame+0x10c word
 * (rsp+0x13c in IsYashiroActive, rsp+0x17c in YashiroAccessBonusAmurita,
 * whose traced body never calls the glue at all). Treat it as a return
 * descriptor/counter, not as proof that a binding answered true or false.
 * WORLD_RESEARCH_NA marks the entry observation, where no return exists
 * yet. */
#define WORLD_RESEARCH_NA (-2)
#define WORLD_RESEARCH_LOG_MAX 64u
/* Native interaction-pipeline offsets; see the module header for the RVAs
 * that read and write each one. */
#define N4P_GADGET_AREAS    0x98u   /* gadget: interaction-area container */
#define N4P_AREA_STRIDE     0x120u  /* area entry stride */
#define N4P_AREA_ISINT      0x2au   /* container+0x2a: is-interacting byte */
#define N4P_AREA_BBOX_VALID 0x4c0u  /* container+0x4c0: bbox valid/enable */
#define N4P_CHAR_GET_RVA    0x755dc0u  /* local player character getter */
#define N4P_CHAR_INTERACT   0x230u  /* char+0x230: interaction component */
#define N4P_INTERACT_INNER  0x8u    /* component+0x8: inner object */
#define N4P_INTERACT_TARGET 0x510u  /* inner+0x510: current interaction target */
typedef struct {
    uintptr_t obj;          /* *0x14191e290, 0 when absent/unreadable */
    uint32_t kind;          /* obj+4 word, 0xffffffff when unreadable */
    uintptr_t rich;         /* obj+0x380 raw value, 0 when unreadable */
    uint32_t shrine_id;     /* rich+0x1a68, 0xffffffff when unreadable */
    int32_t access;         /* rich+0x1a82 byte, -1 when unreadable */
    int32_t role;           /* manager+0x5b0, -1 without a manager */
    int32_t coop626;        /* manager+0x626 */
    int32_t active629;      /* manager+0x629 */
    int32_t tokoyo660;      /* manager+0x660 */
    int32_t done662;        /* manager+0x662 */
    uintptr_t areas;        /* obj+0x98 interaction-area container, 0 unread */
    uint32_t area_en;       /* areas+0x4c0 byte, 0xff when unreadable */
    uint32_t isint0;        /* areas+0x2a byte, 0xff when unreadable */
    uint32_t act;           /* active bytes of area entries 0..3 packed,
                             * 0xffffffff when unreadable */
    uintptr_t itarget;      /* local player's interaction target, 0 unread */
    uint32_t flow658;       /* manager+0x658 shrine-menu flow state */
    uint32_t blocked;       /* manager+0x5c8/0x5ca/0x5cb bytes packed */
    uintptr_t menu_weak;    /* manager+0xa90 stored shrine weak pointer */
    int32_t native_return;  /* eax of the native call, WORLD_RESEARCH_NA at
                             * entry; not established as the Lua boolean */
    uint32_t seen;          /* bookkeeping, excluded from the compare */
    uint32_t logged;        /* change lines emitted (hard cap) */
    uint32_t capped;        /* suppression notice emitted */
} WorldResearchObs;

static void world_research_observe(const Nioh4pCoreApi *core,
                                   const char *site, WorldResearchObs *prev,
                                   int32_t native_return)
{
    WorldResearchObs cur;
    uint8_t *m;
    memset(&cur, 0, sizeof cur);
    cur.native_return = native_return;
    {
        uint8_t *const *slot =
            (uint8_t *const *)(core->game_base + N4P_SELECTED_RVA);
        uint8_t *obj = core->readable(slot, sizeof *slot) ? *slot : NULL;
        uint8_t *rich = NULL;
        cur.obj = (uintptr_t)obj;
        if (obj && core->readable(obj, 6))
            cur.kind = *(const uint16_t *)(obj + N4P_SEL_KIND);
        else
            cur.kind = 0xffffffffu;
        if (obj && core->readable(obj, 0x388))
            rich = *(uint8_t **)(obj + N4P_SEL_RICH);
        cur.rich = (uintptr_t)rich;
        if (rich) {
            if (core->readable(rich + N4P_SHRINE_ID, 4))
                cur.shrine_id = *(const uint32_t *)(rich + N4P_SHRINE_ID);
            else
                cur.shrine_id = 0xffffffffu;
            cur.access = core->readable(rich + N4P_SHRINE_ACC, 1) ?
                         rich[N4P_SHRINE_ACC] : -1;
        } else {
            cur.shrine_id = 0xffffffffu;
            cur.access = -1;
        }
        /* Interaction-area state. The shrine prompt is not driven by any of
         * the three bindings; it lives in the native interaction pipeline,
         * so record the fields that gate it (RVAs in the module header):
         * the area container at gadget+0x98, its bbox-valid byte at +0x4c0
         * (written by the per-frame update 0x140774890), the active byte at
         * container+0x28+i*0x120 (Interaction::ActivateArea writes entry+8)
         * and the is-interacting byte at +0x2a. */
        if (obj && core->readable(obj, 0xa0)) {
            uint8_t *areas = *(uint8_t **)(obj + N4P_GADGET_AREAS);
            int i;
            cur.areas = (uintptr_t)areas;
            if (areas && core->readable(areas + N4P_AREA_ISINT, 1))
                cur.isint0 = areas[N4P_AREA_ISINT];
            else
                cur.isint0 = 0xffu;
            if (areas && core->readable(areas + N4P_AREA_BBOX_VALID, 1))
                cur.area_en = areas[N4P_AREA_BBOX_VALID];
            else
                cur.area_en = 0xffu;
            cur.act = 0xffffffffu;
            if (areas &&
                core->readable(areas + 0x28 + 3 * N4P_AREA_STRIDE, 1)) {
                cur.act = 0;
                for (i = 0; i < 4; i++)
                    cur.act |= (uint32_t)areas[0x28 + i * N4P_AREA_STRIDE]
                               << (8 * i);
            }
        }
    }
    /* The local player's interaction target: 0x140755dc0(0) is the same
     * character getter the bindings themselves call, so it is safe on the
     * game thread that runs this observer. The component chain mirrors the
     * native target compare at 0x1407400b1/0x140944587. */
    {
        void *(*get_char)(void *) =
            (void *(*)(void *))(core->game_base + N4P_CHAR_GET_RVA);
        void *ch = get_char(0);
        if (ch && core->readable((uint8_t *)ch + N4P_CHAR_INTERACT, 8)) {
            void *comp = *(void **)((uint8_t *)ch + N4P_CHAR_INTERACT);
            if (comp &&
                core->readable((uint8_t *)comp + N4P_INTERACT_INNER, 8)) {
                void *inner =
                    *(void **)((uint8_t *)comp + N4P_INTERACT_INNER);
                if (inner && core->readable((uint8_t *)inner +
                                            N4P_INTERACT_TARGET, 8))
                    cur.itarget = (uintptr_t)*(void **)
                        ((uint8_t *)inner + N4P_INTERACT_TARGET);
            }
        }
    }
    m = n4p_world_manager(core);
    cur.role      = m ? m[N4P_MGR_ROLE]      : -1;
    cur.coop626   = m ? m[N4P_MGR_COOP_ON]   : -1;
    cur.active629 = m ? m[N4P_MGR_SHRINE_ON] : -1;
    cur.tokoyo660 = m ? m[N4P_MGR_TOKOYO]    : -1;
    cur.done662   = m ? m[N4P_MGR_COOP_DONE] : -1;
    if (m) {
        cur.flow658 = core->readable(m + 0x658, 4) ?
                      *(const uint32_t *)(m + 0x658) : 0xffffffffu;
        cur.blocked = core->readable(m + 0x5c8, 4) ?
                      (uint32_t)m[0x5c8] | ((uint32_t)m[0x5ca] << 8) |
                      ((uint32_t)m[0x5cb] << 16) : 0xffffffffu;
        cur.menu_weak = core->readable(m + N4P_MGR_MENU_WEAK, 8) ?
                        (uintptr_t)*(void **)(m + N4P_MGR_MENU_WEAK) : 0;
    } else {
        cur.flow658 = 0xffffffffu;
        cur.blocked = 0xffffffffu;
        cur.menu_weak = 0;
    }
    if (prev->seen &&
        prev->obj == cur.obj && prev->kind == cur.kind &&
        prev->rich == cur.rich && prev->shrine_id == cur.shrine_id &&
        prev->access == cur.access && prev->role == cur.role &&
        prev->coop626 == cur.coop626 && prev->active629 == cur.active629 &&
        prev->tokoyo660 == cur.tokoyo660 && prev->done662 == cur.done662 &&
        prev->native_return == cur.native_return &&
        prev->areas == cur.areas && prev->area_en == cur.area_en &&
        prev->isint0 == cur.isint0 && prev->act == cur.act &&
        prev->itarget == cur.itarget && prev->flow658 == cur.flow658 &&
        prev->blocked == cur.blocked && prev->menu_weak == cur.menu_weak)
        return;
    /* Carry the bookkeeping across, then hard-bound the output: past
     * WORLD_RESEARCH_LOG_MAX changes one notice is logged and this site/stage
     * goes quiet, so even a tuple alternating every frame is bounded. Each
     * call site keeps its own state for its own enter and result stages. */
    {
        uint32_t logged = prev->logged;
        uint32_t capped = prev->capped;
        *prev = cur;
        prev->seen = 1;
        prev->logged = logged + 1;
        prev->capped = capped;
        if (logged >= WORLD_RESEARCH_LOG_MAX) {
            if (!capped) {
                prev->capped = 1;
                core->log("shrines-research: %s change log capped at %u "
                          "lines, suppressing further changes", site,
                          (unsigned)WORLD_RESEARCH_LOG_MAX);
            }
            return;
        }
    }
    core->log("shrines-research: %s obj=%p kind=%u rich=%p id=%u access=%d "
              "role=%d coop626=%d active629=%d tokoyo660=%d done662=%d "
              "native_return=%d areas=%p en=%u isint=%u act=%u "
              "itarget=%p flow658=%u blk=%03x menu=%p",
              site, (void *)cur.obj, cur.kind, (void *)cur.rich, cur.shrine_id,
              cur.access, cur.role, cur.coop626, cur.active629,
              cur.tokoyo660, cur.done662, cur.native_return,
              (void *)cur.areas, cur.area_en, cur.isint0, cur.act,
              (void *)cur.itarget, cur.flow658, cur.blocked,
              (void *)cur.menu_weak);
}

/* Mission::YashiroAccessBonusAmurita 0x949090 (name from the script-binding
 * table at image 0x120eaa0): push rbx; push rdi; sub rsp,0x1a8; mov
 * cookie,%rax. The replaced prologue is 17 bytes; only the cookie load is
 * RIP-relative (to 0x1417f7f98, the same security-cookie slot every other
 * world hook rebuilds), and no branch in the function body re-enters the
 * replaced bytes. */
static unsigned relocate_world_predicate(uint8_t *out, uintptr_t game_base)
{
    static const uint8_t head[] = {0x40, 0x53, 0x57, 0x48, 0x81, 0xec,
                                   0xa8, 0x01, 0x00, 0x00};
    return n4p_world_reloc_cookie(out, head, sizeof head, game_base);
}
#endif /* NIOH3P_RESEARCH */

/* ------------------------------- IsYashiroActive ------------------------ */
/* Mission::IsYashiroActive: sub rsp,0x158; mov cookie,%rax */
static unsigned relocate_world_active(uint8_t *out, uintptr_t game_base)
{
    static const uint8_t head[] = {0x48, 0x81, 0xec, 0x58, 0x01, 0x00, 0x00};
    return n4p_world_reloc_cookie(out, head, sizeof head, game_base);
}

static int (*g_active_orig)(void *);

static int det_shrine_active_impl(void *arg)
{
    N4pShrine s;
    const Nioh4pCoreApi *core = g_core;
    uint8_t *m;
    uint8_t coop;
    uint8_t done;
    int native;
    if (!n4p_world_coop(core))
        return g_active_orig(arg);
    if (!n4p_selected_shrine(core, &s))
        return g_active_orig(arg);
    m = n4p_world_manager(core);
    if (!m)
        return g_active_orig(arg);
    if (!n4p_shrine_usable(core, &s)) {
        static uint32_t logged_id = 0xffffffffu;
        int access = core->readable(s.rich + N4P_SHRINE_ACC, 1) ?
                     s.rich[N4P_SHRINE_ACC] : -1;
        if (logged_id != s.id) {
            logged_id = s.id;
            core->log("shrines: shrine %u not available (access=%d)",
                      s.id, access);
        }
        return g_active_orig(arg);
    }
    if (!core->readable(m + N4P_MGR_SHRINE_ON, 1) || !m[N4P_MGR_SHRINE_ON]) {
        static uint32_t logged_id = 0xffffffffu;
        if (logged_id != s.id) {
            logged_id = s.id;
            core->log("shrines: shrine %u blocked by the mission active flag",
                      s.id);
        }
        return g_active_orig(arg);
    }
    if (!core->readable(m + N4P_MGR_COOP_ON, 1) ||
        !core->readable(m + N4P_MGR_COOP_DONE, 1))
        return g_active_orig(arg);
    coop = m[N4P_MGR_COOP_ON];
    done = m[N4P_MGR_COOP_DONE];
    if (!coop && !done) {
        native = g_active_orig(arg);
        if (native) {
            static uint32_t logged_id = 0xffffffffu;
            if (logged_id != s.id) {
                logged_id = s.id;
                core->log("shrines: shrine %u usable in co-op "
                          "(coop=0 done=0, native_return=%d)", s.id, native);
            }
        }
        return native;
    }

    /* The binding writes its answer into the script argument (0x140ddfd00 at
     * 0x140948941), so returning 1 from this detour cannot change what the
     * mission script sees. Clear the co-op latches only for the duration of
     * the native predicate, then restore them: the native code computes and
     * writes the answer itself. The native body reads 0x662 only when
     * manager+0x660 (Tokoyo) is set, so 0x662 is cleared only then. */
    m[N4P_MGR_COOP_ON] = 0;
    if (m[N4P_MGR_TOKOYO]) {
        m[N4P_MGR_COOP_DONE] = 0;
        __asm__ volatile("" ::: "memory");
        native = g_active_orig(arg);
        __asm__ volatile("" ::: "memory");
        m[N4P_MGR_COOP_DONE] = done;
    } else {
        __asm__ volatile("" ::: "memory");
        native = g_active_orig(arg);
        __asm__ volatile("" ::: "memory");
    }
    m[N4P_MGR_COOP_ON] = coop;
    {
        static uint32_t logged_id = 0xffffffffu;
        if (logged_id != s.id) {
            logged_id = s.id;
            core->log("shrines: shrine %u usable in co-op "
                      "(cleared coop=%u done=%u tokoyo=%u, native_return=%d)",
                      s.id, coop, done,
                      (unsigned)m[N4P_MGR_TOKOYO], native);
        }
    }
    return native;
}

#ifdef NIOH3P_RESEARCH
/* RESEARCH-ONLY wrapper around the shrine-active detour: it observes the
 * manager/selected-shrine/interaction-area state at entry (before any gate,
 * so a client outside the 3+ co-op gate - the observed HUD-only 4P sample
 * with manager+0x626 = 0 - still reports what the native body saw) and the
 * raw word the native call returned in eax (the frame+0x10c descriptor the
 * completion glue increments; text.dis does not establish it as the Lua
 * boolean), then returns the detour's result unchanged. */
static int det_shrine_active(void *arg)
{
    static WorldResearchObs entry_prev;
    static WorldResearchObs result_prev;
    int r;
    if (!g_core->trace)
        return det_shrine_active_impl(arg);
    world_research_observe(g_core, "yashiro-active enter", &entry_prev,
                           WORLD_RESEARCH_NA);
    r = det_shrine_active_impl(arg);
    world_research_observe(g_core, "yashiro-active result", &result_prev, r);
    return r;
}
#else
#define det_shrine_active det_shrine_active_impl
#endif

/* ---------------------- research-only observational hooks --------------- */
#ifdef NIOH3P_RESEARCH
/* RESEARCH-ONLY: the menu binding detour only observes and logs; a player
 * build installs no menu detour at all. */
static int (*g_menu_orig)(void *);

static int det_shrine_menu(void *arg)
{
    N4pShrine s;
    uint8_t *m;
    const Nioh4pCoreApi *core = g_core;
    const int have = n4p_selected_shrine(core, &s);
    const int r = g_menu_orig(arg);

    if (!have || !n4p_world_coop(core))
        return r;
    m = n4p_world_manager(core);
    if (m && core->readable(m + N4P_MGR_MENU_WEAK, 8) &&
        *(uint8_t **)(m + N4P_MGR_MENU_WEAK))
        core->log("shrines: shrine %u open-menu binding ran, weak-ref slot "
                  "now holds %p (the native body only stores the selected "
                  "gadget; it proves no menu opened)", s.id,
                  *(void **)(m + N4P_MGR_MENU_WEAK));
    return r;
}

/* RESEARCH-ONLY wrapper around Mission::YashiroAccessBonusAmurita (0x949090,
 * name from the script-binding table at image 0x120eaa0). .text contains no
 * direct call to the address, so it is reached the way the other yashiro
 * bindings are (script frame in rcx, the entry glue 0x140ddf4e0 at
 * 0x1409490b4, frame base rsp+0x70). The detour is observation-only: it
 * always calls the original and returns its return word unchanged. That word
 * is movzwl 0x17c(%rsp) - the frame+0x10c field of its own script frame -
 * and unlike IsYashiroActive the traced body shows no 0x140ddfd00 completion
 * call, so nothing in text.dis establishes what writes it or whether it is
 * ever the Lua boolean; the log records it verbatim as native_return and
 * proves nothing about the binding's answer. */
static int (*g_pred_orig)(void *);

static int det_shrine_predicate(void *arg)
{
    static WorldResearchObs entry_prev;
    static WorldResearchObs result_prev;
    int r;
    if (!g_core->trace)
        return g_pred_orig(arg);
    world_research_observe(g_core, "yashiro-amurita enter", &entry_prev,
                           WORLD_RESEARCH_NA);
    r = g_pred_orig(arg);
    world_research_observe(g_core, "yashiro-amurita result", &result_prev, r);
    return r;
}
#endif

/* -------------------------------- module glue --------------------------- */
static unsigned relocate_world_active_wrap(uint8_t *out)
{
    return relocate_world_active(out, g_core ? g_core->game_base : 0);
}
#ifdef NIOH3P_RESEARCH
static unsigned relocate_predicate_wrap(uint8_t *out)
{
    return relocate_world_predicate(out, g_core ? g_core->game_base : 0);
}
#endif

static Nioh4pHook g_shrine_hooks[] = {
    {0x9488a0, 14, "shrine active", (void *)det_shrine_active,
     (void **)&g_active_orig,
     {0x48, 0x81, 0xec, 0x58, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05,
      0xea, 0xf6, 0xea, 0x00}, relocate_world_active_wrap},
#ifdef NIOH3P_RESEARCH
    {0x949620, 18, "shrine menu", (void *)det_shrine_menu,
     (void **)&g_menu_orig,
     {0x40, 0x57, 0x48, 0x81, 0xec, 0x60, 0x01, 0x00, 0x00,
      0x48, 0xc7, 0x44, 0x24, 0x38, 0xfe, 0xff, 0xff, 0xff}, NULL},
    {0x949090, 17, "shrine predicate", (void *)det_shrine_predicate,
     (void **)&g_pred_orig,
     {0x40, 0x53, 0x57, 0x48, 0x81, 0xec, 0xa8, 0x01, 0x00, 0x00,
      0x48, 0x8b, 0x05, 0xf7, 0xee, 0xea, 0x00}, relocate_predicate_wrap},
#endif
};

static Nioh4pAddon g_shrines_addon = {
    NIOH4P_ADDON_ABI, sizeof g_shrines_addon, NIOH4P_SHRINES,
    0 /* set in Init from config */, g_shrine_hooks,
    sizeof g_shrine_hooks / sizeof g_shrine_hooks[0],
    0, NULL, NULL
};

const Nioh4pAddon *__cdecl Nioh4pAddonInit(const Nioh4pCoreApi *core,
                                           HMODULE module)
{
    int enabled = 1;
    if (!core || core->version != NIOH4P_ADDON_ABI ||
        core->size < sizeof *core)
        return NULL;
    if (!n4p_config_int(core, module, "enabled", "1", 0, 1, &enabled))
        return NULL;
    g_core = core;
    g_shrines_addon.enabled = (uint32_t)enabled;
    return &g_shrines_addon;
}
