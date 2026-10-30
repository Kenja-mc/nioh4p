/* nioh4p-kodama: extracted Mission::SetStrayKodama co-op insertion fallback
 * plus research-only HasStrayKodama observation. Extracted from the combined
 * proxy's "shared world" section; see src/addons/world_helpers.h for the
 * field offsets and docs/RE_TOKOYO_COOP.md for the reverse-engineering notes.
 *
 * Owned native hooks:
 *   0x948e50 Mission::SetStrayKodama - the co-op insertion fallback below.
 *   0x947f60 HasStrayKodama (research build only) - observation of the
 *     lookup; see the research block below.
 * NOT owned here: shrine active/access/menu hooks belong to nioh4p-shrines
 * (access) and nioh4p-shrine-respawn (checkpoint mirror).
 *
 * Mission::SetStrayKodama accepts the host (role 0) and a Tokoyo visitor
 * (role 1 while manager+0x660 is set); a shrine visitor is rejected by that
 * native role gate at 0x140948e90..0x140948eaa, so the native insertion
 * 0x905bc0(save, collection id) never runs on that client. Only for that
 * rejected case, when this client's save still lacks the selected Kodama's
 * ID, does the hook run exactly that insertion BEFORE the binding (after
 * verifying the plaintext prologue bytes of 0x905bc0); the host and a Tokoyo
 * visitor keep the original order and the native path. That matches the
 * native accept ordering (insertion first, then the completion tail
 * 0x1408edb60 -> script VM+0x328), so the tail always sees the new save
 * state. 0x905bc0 deduplicates against the save list, marks the save dirty
 * (movb $1,(save) at 0x140905dd2, the same dirty byte 0x8ff270 writes at
 * 0x1408ff283), is per client and per save, never touches another player's
 * save and sends no packet.
 *
 * This extraction is NOT end-to-end proof that the feature works. The
 * research block now recomputes the HasStrayKodama predicate (the pushed
 * Boolean text.dis maps to "collected or absent") and dumps the mission's
 * stray-Kodama placements with each client's save state once per map, which
 * is the live read that identifies the uncollected Kodamas a co-op run can
 * collect. Visibility, separate collection and reload persistence still
 * need the parent-run protocol (docs/RE_TOKOYO_COOP.md). */
#include "world_helpers.h"

static const Nioh4pCoreApi *g_core;

/* -------------------------------- SetStrayKodama ------------------------ */
/* Mission::SetStrayKodama: push rbx; push r14; sub rsp,0x148; mov cookie,%rax */
static unsigned relocate_world_kodama(uint8_t *out)
{
    static const uint8_t head[] = {0x40, 0x53, 0x41, 0x56,
                                   0x48, 0x81, 0xec, 0x48, 0x01, 0x00, 0x00};
    return n4p_world_reloc_cookie(out, head, sizeof head, g_core->game_base);
}

static int (*g_kodama_orig)(void *);

static int det_set_stray_kodama(void *arg)
{
    /* Plaintext prologue of 0x140905bc0: push r12; sub $0x40,%rsp;
     * movslq 0x15c2e4(%rcx),%r9; mov %rcx,%r12. Executable memory alone does
     * not prove the function (game code pages can be encrypted/swapped), so
     * the bytes are compared before the direct call. */
    static const uint8_t insert_head[] = {
        0x41, 0x54, 0x48, 0x83, 0xec, 0x40,
        0x4c, 0x63, 0x89, 0xe4, 0xc2, 0x15, 0x00,
        0x4c, 0x8b, 0xe1
    };
    const Nioh4pCoreApi *core = g_core;
    uint8_t *m = n4p_world_manager(core);
    /* Only shrine visitors miss the native insertion. Keep the host and
     * Tokoyo visitor on the native path without extra save/map scans. */
    int rejected_visitor = m && m[N4P_MGR_ROLE] == 1 && !m[N4P_MGR_TOKOYO];
    int inspect = rejected_visitor;
#ifdef NIOH3P_RESEARCH
    if (core->trace)
        inspect = 1;
#endif
    if (!inspect)
        return g_kodama_orig(arg);
    uint8_t *save = n4p_world_save(core);
    uint32_t id = 0;
    const int have = save && n4p_world_coop(core) &&
                     n4p_selected_kodama(core, &id);
    rejected_visitor = rejected_visitor && have;
    int pre = -1;
    int before = -1;
    if (have) {
        pre = n4p_kodama_collected(core, save, id);
        if (pre >= 0)
            before = *(const int *)(save + N4P_KODAMA_COUNT);
    }
    if (rejected_visitor) {
        /* Run the same insertion 0x905bc0(save, id) BEFORE the binding: the
         * native accept path also inserts first and runs the completion tail
         * (0x1408edb60 -> script VM+0x328) after it, so the tail sees the new
         * save state. The insertion deduplicates against the save list and
         * marks the save dirty (movb $1,(save) at 0x140905dd2, the byte
         * 0x8ff270 writes at 0x1408ff283). */
        if (pre == 0 && before >= 0 && before < N4P_KODAMA_MAX) {
            const uint8_t *insert_site =
                (const uint8_t *)(core->game_base + 0x905bc0);
            if (core->readable(insert_site, sizeof insert_head) &&
                !memcmp(insert_site, insert_head, sizeof insert_head)) {
                ((void (*)(uint8_t *, uint32_t))(core->game_base + 0x905bc0))
                    (save, id);
                if (n4p_kodama_collected(core, save, id) == 1)
                    core->log("kodama: Kodama %u credited through 0x905bc0",
                              id);
                else
                    core->log("kodama: Kodama %u insertion did not land",
                              id);
            }
        }
    }
    int r = g_kodama_orig(arg);
#ifdef NIOH3P_RESEARCH
    if (core->trace && have) {
        const int now = n4p_kodama_collected(core, save, id);
        static uint32_t logged_id = 0xffffffffu;
        static int logged_pre = -2;
        static int logged_now = -2;
        if (logged_id != id || logged_pre != pre || logged_now != now) {
            logged_id = id;
            logged_pre = pre;
            logged_now = now;
            /* Local observations only; inspect every client's count to
             * establish whether a native collection event is shared. */
            if (now == 1 && pre == 0)
                core->log("kodama: Kodama %u credited on this client "
                          "(save count %d -> %d)", id, before,
                          *(const int *)(save + N4P_KODAMA_COUNT));
            else if (now == 1 && pre == 1)
                core->log("kodama: Kodama %u already collected before "
                          "the binding (save count %d unchanged)", id,
                          before);
            else if (now == 0)
                core->log("kodama: Kodama %u not collected after the "
                          "binding (save count %d)", id, before);
            else
                core->log("kodama: Kodama %u save unreadable after the "
                          "binding", id);
        }
    }
#endif
    return r;
}

#ifdef NIOH3P_RESEARCH
/* RESEARCH-ONLY observation of Mission::HasStrayKodama (0x947f60). text.dis
 * shows the binding's pushed Boolean (the byte at entry rsp+0x20, wrapped by
 * 0x140ddfd00) is 1 when the selected gadget is missing or its collection id
 * is already in this client's save, and 0 when an uncollected stray exists;
 * the C return word is only the Lua value count, so the detour recomputes the
 * predicate from the same native inputs (selected gadget 0x191e290, gadget
 * map manager+0x40, local save 0x189f408) instead of trusting the word. The
 * detour also dumps the gadget map's category-0x44c nodes once per map: that
 * is the live read that lists every stray Kodama id of the loaded mission
 * with this client's save state, so the uncollected entries (save=0) are the
 * candidates a co-op run can collect. Read-only: no save write, no packet. */
static int (*g_has_orig)(void *);

/* Preorder DFS over the gadget std::map, same node layout as
 * n4p_gadget_node (header+8 root, +0/+0x10 children, sentinel = header,
 * +0x20 key low 32, +0x28 data); depth and visit guards bound a corrupted
 * ring. Logs every Kodama node's collection id with this client's save
 * state (-1 unreadable, 0 uncollected, 1 collected). */
static void kodama_dump_walk(const Nioh4pCoreApi *core, uint8_t *node,
                             uint8_t *sentinel, uint8_t *save,
                             unsigned *visited, int depth)
{
    uint8_t *data;
    uint32_t id;
    if (node == sentinel || depth > N4P_TREE_MAX_DEPTH ||
        *visited >= 4096u || !core->readable(node, 0x30))
        return;
    (*visited)++;
    data = *(uint8_t **)(node + 0x28);
    if (data && core->readable(data, 0x10) &&
        core->readable(node + 0x20, 0x10) &&
        *(const uint32_t *)(node + 0x20) == N4P_KODAMA_KEY &&
        core->readable(data + 0xb4, 4)) {
        id = *(const uint32_t *)(data + 0xb4);
        if (id != 0xffffffffu)
            core->log("kodama-research: map: Kodama %u save=%d", id,
                      n4p_kodama_collected(core, save, id));
    }
    kodama_dump_walk(core, *(uint8_t **)(node), sentinel, save, visited,
                     depth + 1);
    kodama_dump_walk(core, *(uint8_t **)(node + 0x10), sentinel, save,
                     visited, depth + 1);
}

/* One dump per (manager, map header) pair: a mission load rebuilds the map,
 * so each mission entry logs the placement list once. If a rebuild reuses
 * both addresses verbatim the dump is skipped for that mission; the log then
 * shows only the per-id lookup lines. */
static void kodama_maybe_dump_map(const Nioh4pCoreApi *core, uint8_t *m,
                                  uint8_t *save)
{
    static uint8_t *dumped_mgr;
    static uint8_t *dumped_header;
    uint8_t *header;
    uint8_t *root;
    unsigned visited = 0;
    if (!core->readable(m + N4P_MGR_KODAMA_MAP, 8))
        return;
    header = *(uint8_t **)(m + N4P_MGR_KODAMA_MAP);
    if (!header || header == dumped_header || m == dumped_mgr ||
        !core->readable(header, 16))
        return;
    dumped_mgr = m;
    dumped_header = header;
    root = *(uint8_t **)(header + 8);
    kodama_dump_walk(core, root, header, save, &visited, 0);
    core->log("kodama-research: map dump done (%u nodes visited)", visited);
}

static int det_has_stray_kodama(void *arg)
{
    uint32_t id = 0;
    uint8_t *m;
    uint8_t *save;
    int have;
    int r;
    if (!g_core->trace)
        return g_has_orig(arg);
    m = n4p_world_manager(g_core);
    save = n4p_world_save(g_core);
    if (m && save)
        kodama_maybe_dump_map(g_core, m, save);
    have = n4p_selected_kodama(g_core, &id);
    r = g_has_orig(arg);
    if (!have)
        return r;
    {
        static uint32_t logged_id = 0xffffffffu;
        static int logged_c = -2;
        const int c = n4p_kodama_collected(g_core, save, id);
        if (logged_id != id || logged_c != c) {
            logged_id = id;
            logged_c = c;
            /* bool= is the recomputed native Boolean for a found gadget
             * (0 visible uncollected, 1 hidden/collected, -1 save
             * unreadable); -> is the verbatim return word. */
            g_core->log("kodama-research: Kodama %u lookup -> %d bool=%d "
                        "(save=%d role=%d tokoyo=%d)",
                        id, r, c == 0 ? 0 : (c < 0 ? -1 : 1), c,
                        m ? (int)m[N4P_MGR_ROLE] : -1,
                        m ? (int)m[N4P_MGR_TOKOYO] : -1);
        }
    }
    return r;
}
#endif

/* -------------------------------- module glue --------------------------- */
static Nioh4pHook g_kodama_hooks[] = {
    {0x948e50, 18, "kodama collect", (void *)det_set_stray_kodama,
     (void **)&g_kodama_orig,
     {0x40, 0x53, 0x41, 0x56, 0x48, 0x81, 0xec, 0x48, 0x01, 0x00, 0x00,
      0x48, 0x8b, 0x05, 0x36, 0xf1, 0xea, 0x00}, relocate_world_kodama},
#ifdef NIOH3P_RESEARCH
    {0x947f60, 23, "kodama lookup", (void *)det_has_stray_kodama,
     (void **)&g_has_orig,
     {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18,
      0x48, 0x89, 0x74, 0x24, 0x20,
      0x57, 0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00}, NULL},
#endif
};

static Nioh4pAddon g_kodama_addon = {
    NIOH4P_ADDON_ABI, sizeof g_kodama_addon, NIOH4P_KODAMA,
    0 /* set in Init from config */, g_kodama_hooks,
    sizeof g_kodama_hooks / sizeof g_kodama_hooks[0],
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
    g_kodama_addon.enabled = (uint32_t)enabled;
    return &g_kodama_addon;
}
