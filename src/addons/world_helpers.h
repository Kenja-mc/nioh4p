/* Shared little world helpers for the world-slice add-ons (shrines, kodama).
 * Header-only: every function is static, so each translation unit keeps its
 * own copy and no cross-DLL symbol is exported. The field offsets mirror the
 * combined proxy source (src/proxy/nioh3p_proxy.c, "shared world" section).
 * Every pointer is guarded by core->readable before use. */
#ifndef NIOH4P_WORLD_HELPERS_H
#define NIOH4P_WORLD_HELPERS_H
#include "runtime.h"

/* Online manager fields (manager = [base+0x18715e0]). */
#define N4P_MGR_ROLE        0x5b0u   /* 0 host, 1 visitor */
#define N4P_MGR_CHECKPOINT  0x114u   /* u32 last recorded shrine id */
#define N4P_MGR_COOP_ON     0x626u   /* co-op latch read by IsYashiroActive */
#define N4P_MGR_SHRINE_ON   0x629u   /* mission shrine-active flag */
#define N4P_MGR_TOKOYO      0x660u   /* Tokoyo visitor mode latch */
#define N4P_MGR_COOP_DONE   0x662u   /* shrine-used latch (Tokoyo) */
#define N4P_MGR_MENU_WEAK   0xa90u   /* weak ptr to the open shrine menu */
#define N4P_MGR_KODAMA_MAP  0x40u    /* [mgr+0x40] = std::map header */
/* Selected gadget (slot = [base+0x191e290]). */
#define N4P_SEL_KIND        0x4u     /* u16 kind; 2 = shrine/Kodama gadget */
#define N4P_SEL_RICH        0x380u   /* richdata pointer */
#define N4P_SHRINE_ID       0x1a68u  /* richdata: shrine id */
#define N4P_SHRINE_ACC      0x1a82u  /* richdata: access flag byte */
/* Local save ([base+0x189f408]). */
#define N4P_SAVE_SHRINE_BITS 0x15682cu /* u32 visited shrine bits, 0..31 */
#define N4P_KODAMA_COUNT     0x15c2e4u
#define N4P_KODAMA_IDS       0x15c2e8u
#define N4P_KODAMA_KEY       0x44cu    /* gadget-map node type tag */
#define N4P_KODAMA_MAX       512
#define N4P_TREE_MAX_DEPTH   64
/* Security-cookie slot every world hook relocation rebuilds. */
#define N4P_COOKIE_RVA       0x17f7f98u

typedef struct {
    uint8_t *obj;    /* selected gadget object */
    uint8_t *rich;   /* its richdata block */
    uint32_t id;     /* richdata shrine id */
} N4pShrine;

/* Online manager with the co-op fields mapped and readable. */
static inline uint8_t *n4p_world_manager(const Nioh4pCoreApi *core)
{
    uint8_t *m = n4p_manager(core);
    if (!m || !core->readable(m + N4P_MGR_ROLE, 0xb4))
        return NULL;
    return m;
}

/* Live party size from the session root slot records: record byte 0xe8 is 2
 * for a connected member, 0 empty, 3 closed. */
static inline unsigned n4p_world_party(const Nioh4pCoreApi *core)
{
    return n4p_party_count(core);
}

/* At least three connected members in any co-op mode. Both the Torii Gate
 * companion path and the shrine-visitor path qualify; solo and stock
 * two-player missions do not. The Tokoyo latch is not required: the Kodama
 * fallback exists precisely for a shrine visitor that the native role gate
 * rejects, and the shrine co-op latch is set in both modes. */
static inline int n4p_world_coop(const Nioh4pCoreApi *core)
{
    return n4p_world_manager(core) && n4p_world_party(core) >= 3;
}

static inline uint8_t *n4p_world_save(const Nioh4pCoreApi *core)
{
    uint8_t *const *p = (uint8_t *const *)(core->game_base + N4P_SAVE_RVA);
    if (!core->readable(p, sizeof *p) || !*p)
        return NULL;
    return *p;
}

static inline int n4p_selected_shrine(const Nioh4pCoreApi *core, N4pShrine *s)
{
    uint8_t *const *slot = (uint8_t *const *)(core->game_base + N4P_SELECTED_RVA);
    uint8_t *obj;
    uint8_t *rich;
    if (!core->readable(slot, sizeof *slot))
        return 0;
    obj = *slot;
    if (!obj || !core->readable(obj, 0x388) ||
        *(const uint16_t *)(obj + N4P_SEL_KIND) != 2)
        return 0;
    rich = *(uint8_t **)(obj + N4P_SEL_RICH);
    if (!rich || !core->readable(rich + N4P_SHRINE_ID, 0x1c))
        return 0;
    s->obj = obj;
    s->rich = rich;
    s->id = *(const uint32_t *)(rich + N4P_SHRINE_ID);
    return 1;
}

/* The selected gadget's own shrine fields: the richdata access flag and an id
 * other than -1. IsYashiroActive additionally needs manager+0x629. The id is
 * not range-limited because 0x8ff270 shifts a 32-bit mask by it; the
 * checkpoint check masks the shift count exactly as the native instruction
 * does. */
static inline int n4p_shrine_usable(const Nioh4pCoreApi *core, const N4pShrine *s)
{
    if (!s->rich)
        return 0;
    if (!core->readable(s->rich + N4P_SHRINE_ACC, 1) || !s->rich[N4P_SHRINE_ACC])
        return 0;
    if (s->id == 0xffffffffu)
        return 0;
    return 1;
}

/* DFS over the gadget std::map (red-black tree) looking for the node whose
 * data pointer is `want`. MSVC std::map header layout, confirmed live
 * (header 0x60dc5b20: left 616172e0 with parent 6161b390, parent 61617830
 * with parent 60dc5b20 = the header, right 6161ac60): [0] is the leftmost
 * node (MIN), [8] the root, [0x10] the rightmost. The native walk at
 * 0x140948ee0 starts at MIN and iterates in-order successors through the
 * parent links (+8). This DFS instead starts at the ROOT: with only
 * left/right links, a preorder DFS from the root is what visits every node;
 * starting at MIN would cover only MIN's subtree and miss the ancestor
 * subtrees. Leaf children point back at the header, which is the sentinel
 * the recursion compares against. The depth guard bounds a corrupted ring. */
static inline uint8_t *n4p_gadget_node(const Nioh4pCoreApi *core, uint8_t *node,
                                uint8_t *sentinel, uint8_t *want, int depth)
{
    uint8_t *data;
    uint8_t *hit;
    if (node == sentinel || depth > N4P_TREE_MAX_DEPTH ||
        !core->readable(node, 0x30))
        return NULL;
    data = *(uint8_t **)(node + 0x28);
    if (data && core->readable(data, 0x10) && *(uint8_t **)data == want)
        return node;
    hit = n4p_gadget_node(core, *(uint8_t **)node, sentinel, want, depth + 1);
    if (hit)
        return hit;
    return n4p_gadget_node(core, *(uint8_t **)(node + 0x10), sentinel, want,
                           depth + 1);
}

/* Collection id of the currently selected Kodama, or 0. The gadget map is the
 * same red-black tree that HasStrayKodama and SetStrayKodama walk. */
static inline int n4p_selected_kodama(const Nioh4pCoreApi *core, uint32_t *id_out)
{
    uint8_t *const *sel = (uint8_t *const *)(core->game_base + N4P_SELECTED_RVA);
    uint8_t *m = n4p_world_manager(core);
    uint8_t *header;
    uint8_t *root;
    uint8_t *node;
    uint8_t *data;
    uint32_t id;
    if (!m || !core->readable(sel, sizeof *sel) || !*sel ||
        !core->readable(m + N4P_MGR_KODAMA_MAP, 8))
        return 0;
    header = *(uint8_t **)(m + N4P_MGR_KODAMA_MAP);
    if (!header || !core->readable(header, 16))
        return 0;
    root = *(uint8_t **)(header + 8);
    node = n4p_gadget_node(core, root, header, *sel, 0);
    if (!node || !core->readable(node + 0x20, 0x10))
        return 0;
    if (*(const uint32_t *)(node + 0x20) != N4P_KODAMA_KEY)
        return 0;
    data = *(uint8_t **)(node + 0x28);
    if (!data || !core->readable(data + 0xb4, 4))
        return 0;
    id = *(const uint32_t *)(data + 0xb4);
    if (id == 0xffffffffu)
        return 0;
    *id_out = id;
    return 1;
}

/* 1 collected, 0 not collected, -1 unreadable. Mirrors 0x8ef1b0's linear scan;
 * the save fields and the 512 cap are the native ones. */
static inline int n4p_kodama_collected(const Nioh4pCoreApi *core, uint8_t *save,
                                uint32_t id)
{
    int n;
    const uint32_t *ids;
    if (!save || !core->readable(save + N4P_KODAMA_COUNT, 4))
        return -1;
    n = *(const int *)(save + N4P_KODAMA_COUNT);
    if (n < 0 || n > N4P_KODAMA_MAX)
        return -1;
    if (!n)
        return 0;
    if (!core->readable(save + N4P_KODAMA_IDS, (size_t)n * 4))
        return -1;
    ids = (const uint32_t *)(save + N4P_KODAMA_IDS);
    for (int i = 0; i < n; i++)
        if (ids[i] == id)
            return 1;
    return 0;
}

/* The world functions are position independent except for the security-cookie
 * load, which every relocation helper rebuilds as a movabs from the same
 * absolute address. */
static inline unsigned n4p_world_reloc_cookie(uint8_t *out, const uint8_t *head,
                                       unsigned head_len, uintptr_t game_base)
{
    uintptr_t cookie = game_base + N4P_COOKIE_RVA;
    memcpy(out, head, head_len);
    out[head_len] = 0x48;               /* movabs cookie,%rax */
    out[head_len + 1] = 0xa1;
    memcpy(out + head_len + 2, &cookie, 8);
    return head_len + 10;
}

#endif
