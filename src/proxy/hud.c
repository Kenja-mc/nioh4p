/* nioh3p core: hud (split from nioh3p_proxy.c) */
#include "nioh3p_internal.h"

/* ------------------------------------------- party HUD: the second row -- */

/* The mission HUD builds two MultiPlayerGauge rows, element ids 0x74 and 0x75.
 * Only the first is drawn. Both carry a live copy of a companion's HP, and the
 * row shows that companion's own name, so the hidden row is a complete second
 * companion row. Its state word at +0x28 holds 0x01000000 while hidden and
 * 0x01010001 when drawn. Setting the two missing bits makes the row appear and
 * it stays up. The HUD has two rows in total: the third row for a fourth
 * player is a separate element (see "the third row" below), and both run only
 * when max_players allows it. Every row action happens on the game thread.
 * Evidence: docs/RE_TOKOYO_COOP.md, "Party HUD". */
#define HUD_ROW_ID_OFF     0x8u
#define HUD_ROW_FLAGS_OFF  0x28u
#define HUD_ROW_SHOWN      0x01010001u

void *(*g_hud_row_ctor)(void *self, int id, int index);

/* 48 89 4c 24 08   mov %rcx,0x8(%rsp)
 * 53               push %rbx
 * 48 83 ec 30      sub $0x30,%rsp
 * 48 c7 44 24 20 fe ff ff ff   movq $-2,0x20(%rsp)
 * The trampoline runs one return address deeper, so both stack displacements
 * move by 8. */
unsigned relocate_hud_row(uint8_t *out)
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
 * changes, so silence from the row code is never ambiguous with "mode off". */


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
 * proxy-owned block C, and lets the class bind C to the companion that
 * neither native row shows.
 *
 * C reaches the class code only as an explicit argument: the assigner
 * 0xc21240 and the helpers 0xc1e310, 0xc1e400, 0xc20c50 ... take the block.
 * The seven methods that pick A or B by element id with a RIP-relative
 * address cannot be pointed at C, so the private vtable carries rewrites of
 * them against C (hud_row3_update_native and friends). Nothing is exchanged
 * with A and no reference node is ever moved.
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
 *   - block+0x30 is not a character pointer but a reference node bound into the
 *     character's release list (see hud_ref_* below); block+0x00 is the row's
 *     state machine (0 empty, 1 bound, 2/3/4 showing/fading, see 0xc1f500);
 *     the party slots at 0x18a0490 (stride 0x18) are nodes of the same kind.
 *   - the empty state block is 0x78 bytes and 0xc1eb10 resets it; the ctor
 *     calls that reset for whichever block its id selects. */
#define HUD_ROW3_ID          0x73
#define HUD_ROW3_SETTLE      60         /* frames with both rows resolved before the build */
#define HUD_ROW3_VTABLE_RVA  0x12729c0u
#define HUD_ROW3_VTABLE_SIZE 0x4f0u     /* measured extent, not the slot count */
#define HUD_ROOT_VTABLE_RVA  0x1277e28u
#define HUD_ROOT_VTABLE_SPAN 0xe0u      /* through visibility slot 0xd8 */
#define HUD_ROW3_BLOCK_A_RVA 0x1b46a90u
#define HUD_ROW3_BLOCK_B_RVA 0x1b46b08u
#define HUD_ROW3_BLOCK_SIZE  0x78u
#define HUD_ROW3_STATE_OFF   0x00u      /* 0 empty, 2 populated */
#define HUD_ROW3_CHAR_OFF    0x30u      /* companion character object */
#define HUD_ROW3_SPRITES_OFF 0x48u      /* four sprite pointers, 0x78-byte block */
#define HUD_ROW3_CONTAINER_OFF 0x60u    /* element+0x60: the row's container sprite */
#define HUD_ROW3_ELEMS_RVA   0x189f400u /* [.] = element registry base */
#define HUD_ROW3_MENU_RVA    0xfa6fd0u  /* returns the menu object */
#define HUD_ROW3_REGISTER_RVA 0xbfacc0u /* (registry, elem) -> bool */
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
/* Native helpers that take the block as an explicit argument, and so work on
 * block C without any exchange of A and C:
 *   0xc21240 (row, arg, block)  the character assigner; its exclusion list is
 *                               the static nodes of A and B (0xc21362/0xc2137d)
 *   0xc1e310 (block)            unbind: unlink the node, hide, state 0
 *   0xdd93e0 (sprite)           the sprite readiness predicate
 * Reference node = block+0x30: {character, prev @+0x38, next @+0x40}, an
 * intrusive doubly linked list whose head is [character+0xf40]. Bind
 * 0x753ad0 pushes at the head, unbind 0x756bc0 unlinks, and the character's
 * release path (0x1407535d7) zeroes every node still linked. */
#define HUD_ROW3_ASSIGN_RVA       0xc21240u
#define HUD_ROW3_RESET_RVA        0xc1e310u
#define HUD_ROW3_SPRITE_READY_RVA 0xdd93e0u
#define HUD_ROW3_REF_OFF          0x30u
#define HUD_ROW3_REF_PREV         0x08u     /* node+0x08 = block+0x38 */
#define HUD_ROW3_REF_NEXT         0x10u     /* node+0x10 = block+0x40 */
#define HUD_ROW3_REF_HEAD         0xf40u    /* character+0xf40 = first node */
#define HUD_ROW3_REF_UNLINK_RVA   0x756bc0u /* unlink a node */
#define HUD_ROW3_MODE_MGR_RVA     0x189f420u /* [.] HUD mode manager: +0x150, +0x154 */

/* Companion choice is the game's: the assigner (0xc21240) walks the character
 * set and takes the first character that is not the local one, has
 *   *(i32*)(char+0xef0) != -1, *(u8*)(char+0x430) != 0,
 *   *(void**)(char+0x240) != NULL, *(i32*)([char+0x240]+0x10) != 1,
 * and is not held by static block A or B (their nodes at 0x141b46ac0 and
 * 0x141b46b38). The character's HP is [[char+0x240]+0x20] of [..+0x18]. */

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
typedef int (*hud_assign_fn)(void *row, void *arg, void *block);   /* 0x140c21240 */
typedef void (*hud_unbind_fn)(void *block);                        /* 0x140c1e310 */

/* Block C, the proxy-owned state block of the third row. Its address is stable
 * for the life of the process, which is what makes its reference node (see
 * hud_ref_*) safe: a node is linked into a character's list by address. */
static uint8_t g_hud_row3_block[HUD_ROW3_BLOCK_SIZE] __attribute__((aligned(8)));
static void *g_hud_row3_vtable;
static void *g_hud_row3_orig[HUD_ROW3_SLOT_COUNT];
static volatile LONG g_hud_row3_active;    /* our element is live: thunks may run */
static int g_hud_row3_released;            /* the release slot already ran */
static volatile LONG g_hud_row3_dead;      /* the game destroyed our element */
static void *g_hud_row3_elem;              /* the element we published */
static int g_hud_row3_attempts;            /* build attempts this session */
static int g_hud_row3_failed;              /* stop trying for this session */
static int g_hud_row3_settle;              /* consecutive settled frames */
static int g_hud_row3_wait_frames = -1;    /* frames spent waiting for sprites */
static DWORD g_hud_row3_last_assign;       /* ms of the last companion search */
static int g_hud_row3_have_pos;            /* the position we last wrote is still in place */
static volatile LONG g_hud_row3_frame_fired;
/* Native row updates and the main HUD's visibility callbacks. */
static void *g_hud_row3_native_orig[4];    /* row update/frame, root b8/d8 */
static volatile LONG g_hud_row3_native_ok; /* slots patched, originals known */
static volatile LONG g_hud_row3_seen_update;
static volatile LONG g_hud_row3_seen_frame;
static volatile LONG g_hud_row3_driven;    /* at least one game-thread drive */
static DWORD g_hud_row3_last_drive;        /* rate limit, ms */
/* The prep/lobby screen builds rows 0x74/0x75 too, so the third row must wait
 * for the mission HUD. The mission loader phase ([manager+0x5d0] == 0xC, the
 * same field `rig.py state` prints as loader) separates the two. */
static volatile LONG g_hud_row_gen;        /* bumped for every native 0x74 build */
static int g_hud_row3_gen = -1;            /* generation our element belongs to */

static void hud_row3_veh_install(void);

uint8_t *hud_row3_a(void) { return g_base + HUD_ROW3_BLOCK_A_RVA; }
uint8_t *hud_row3_b(void) { return g_base + HUD_ROW3_BLOCK_B_RVA; }

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
static int piggy_b8(void *self, uint8_t on);
static int piggy_d8(void *self, uint8_t on);
static void hud_row3_follow_hide(void *self, uint8_t on);

/* ---- reference nodes ----------------------------------------------------
 * block+0x30 is not a pointer, it is a weak reference node {char, prev, next}.
 * The nodes of one character form a doubly linked list whose head is
 * [char+0xf40]; the rows' blocks and the static party slots (0x18-byte
 * stride) are all such nodes, linked by ADDRESS. Bind (0x753ad0) pushes the
 * node at the head, unbind (0x756bc0) unlinks it, and the character's release
 * path zeroes every node still linked, so the holder sees NULL instead of a
 * dangling character.
 *
 * Two things followed from that, and both are why the earlier version crashed:
 *   - a raw `block[+0x30] = character` store never joins the list, so the
 *     character's release could not clear it: the block kept a freed
 *     character and the class update read through its torn-down model
 *     (host: exe+0x765ba0, [[char+0x230]+8]+0x58 = 0);
 *   - exchanging whole blocks moved a node's contents but not its address.
 * Block C therefore never moves, is never exchanged with A, and is only ever
 * handed to class code as the explicit block argument. Its node stays at one
 * address for the life of the process. */
static int hud_ref_consistent(const uint8_t *node)
{
    const uint8_t *const ch = *(const uint8_t *const *)node;
    if (!ch)
        return 1;                   /* unlinked: nothing points at it */
    if (!readable(ch + HUD_ROW3_REF_HEAD, sizeof(void *)))
        return 0;
    const uint8_t *const prev = *(const uint8_t *const *)(node + HUD_ROW3_REF_PREV);
    const uint8_t *const next = *(const uint8_t *const *)(node + HUD_ROW3_REF_NEXT);
    if (prev) {
        if (!readable(prev, 0x18) ||
            *(const uint8_t *const *)(prev + HUD_ROW3_REF_NEXT) != node)
            return 0;
    } else if (*(const uint8_t *const *)(ch + HUD_ROW3_REF_HEAD) != node) {
        return 0;
    }
    if (next && (!readable(next, 0x18) ||
                 *(const uint8_t *const *)(next + HUD_ROW3_REF_PREV) != node))
        return 0;
    return 1;
}

/* Release block C's hold on the world without touching a sprite: unlink its
 * reference node natively and clear the block. For an element that is gone or
 * about to be rebuilt. A node that does not agree with its character's list is
 * NOT cleared: zeroing it would leave the neighbours and the list head pointing
 * into a block we then reuse. The block is left exactly as it is and the row
 * is disabled for the session. Returns 1 when the block is clean. */
static int hud_row3_forget(void)
{
    uint8_t *const node = g_hud_row3_block + HUD_ROW3_REF_OFF;
    if (*(void **)node) {
        if (!hud_ref_consistent(node)) {
            LOG("hud row3: block C's reference node disagrees with its character's "
                "list (char %p prev %p next %p); block left untouched, third row "
                "disabled for this session", *(void **)node,
                *(void **)(node + HUD_ROW3_REF_PREV), *(void **)(node + HUD_ROW3_REF_NEXT));
            g_hud_row3_failed = 1;
            return 0;
        }
        ((void (*)(void *))(g_base + HUD_ROW3_REF_UNLINK_RVA))(node);
    }
    memset(g_hud_row3_block, 0, sizeof g_hud_row3_block);
    g_hud_row3_have_pos = 0;
    g_hud_row3_released = 0;
    return 1;
}

/* ---- the class methods that select block A or B by element id ------------
 * Seven methods of the class pick their block from the element id with a
 * RIP-relative address, so they cannot be pointed at block C. The helpers
 * they call take the block as an argument. These functions are the same
 * code with C substituted, written against the disassembly (0xc1f500 update,
 * 0xc210c0 frame, 0xc1eba0 init, 0xc1e9c0 release, 0xc1f040 / 0xc1f140 the
 * two visibility slots); 0xc1e2d0, the destructor, does not touch a block and
 * stays the game's. Every native they call is checked against its first bytes
 * (hud_row3_code_ok) so a different executable build refuses the feature
 * instead of running mismatched code. */
typedef void (*hn_v_p)(void *);
typedef void (*hn_v_ppp)(void *, void *, void *);
typedef void (*hn_v_pp)(void *, void *);
typedef uint8_t (*hn_b_p)(void *);
typedef uint8_t (*hn_b_pi)(void *, int);
typedef void (*hn_v_pb)(void *, uint8_t);
typedef void (*hn_v_pib)(void *, int, uint8_t);
typedef void (*hn_v_anim)(void *, int, int, float, uint8_t);    /* 0xdd94c0 */
typedef void (*hn_v_text)(void *, int, int, int, int);          /* 0xddadc0 */
typedef void (*hn_v_setf)(void *, int, float);                  /* 0xddc000 */
typedef void *(*hn_p_fii)(float, int, int);                     /* 0xddf270 */
#define HN(type, rva) ((type)(g_base + (rva)))

static const struct { uint32_t rva; uint8_t bytes[8]; } g_hud_row3_native_sig[] = {
    {0xbf3df0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xbf33c0, {0x48, 0x8b, 0x15, 0x39, 0xc0, 0xca, 0x00, 0x48}},
    {0xddf270, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c}},
    {0xc1eb10, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74}},
    {0xc1e310, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74}},
    {0xc1e400, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x57}},
    {0xc1e680, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74}},
    {0xc20c50, {0x48, 0x8b, 0xc4, 0x56, 0x57, 0x48, 0x81, 0xec}},
    {0xc20a00, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c}},
    {0xc20580, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74}},
    {0xc20300, {0x40, 0x53, 0x48, 0x83, 0xec, 0x70, 0x41, 0x83}},
    {0xc1fe60, {0x40, 0x53, 0x48, 0x83, 0xec, 0x70, 0x41, 0x83}},
    {0xc21240, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55}},
    {0xdd9980, {0x88, 0x51, 0x30, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc}},
    {0xddc090, {0x88, 0x51, 0x31, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc}},
    {0xddbf00, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x41, 0x0f}},
    {0xdd94c0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c}},
    {0xddadc0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xdd9480, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0xe8, 0x45}},
    {0xdd93c0, {0x48, 0x83, 0xec, 0x28, 0xe8, 0x07, 0xce, 0xff}},
    {0xdd93e0, {0x48, 0x8b, 0x41, 0x28, 0x48, 0x85, 0xc0, 0x74}},
    {0xdd9400, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b}},
    {0xddcb00, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xddc000, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xdd6660, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xda}},
    {0xdd6700, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xdd66c0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xdd95d0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xddc0a0, {0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54, 0x41, 0x55}},
    {0xdcfb70, {0x4c, 0x8b, 0x15, 0x69, 0xda, 0xb4, 0x00, 0x44}},
    {0xce2c30, {0x48, 0x8b, 0x05, 0xc9, 0xc7, 0xbb, 0x00, 0x48}},
    {0xddf3b0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0x77f9b0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74}},
    {0x7b2c00, {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7}},
    {0x7b2580, {0x4c, 0x8b, 0x81, 0xa8, 0x01, 0x00, 0x00, 0x4d}},
    {0x7b4290, {0x83, 0x39, 0x01, 0x0f, 0x94, 0xc0, 0xc3, 0xcc}},
    {0x765b60, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x56, 0x48, 0x83}},
    {0xc01980, {0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0xc1, 0x48}},
    {0x752640, {0x48, 0x83, 0xec, 0x28, 0x65, 0x48, 0x8b, 0x04}},
    {0xc04270, {0x48, 0x83, 0xec, 0x58, 0x48, 0x8b, 0x05, 0x9d}},
    {0xdda790, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xbf3af0, {0x88, 0x51, 0x28, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc}},
    {0xbf3dd0, {0x88, 0x51, 0x2a, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc}},
    {0xbf30a0, {0x48, 0x8d, 0x05, 0x21, 0x5e, 0x67, 0x00, 0x89}},
    {0x756bc0, {0x48, 0x8b, 0x51, 0x10, 0x48, 0x8b, 0x41, 0x08}},
    {0xc1f500, {0x48, 0x8b, 0xc4, 0x48, 0x81, 0xec, 0x88, 0x00}},
    {0xc210c0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {0xc1eba0, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c}},
    {0xc1e9c0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74}},
    {0xc1f040, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c}},
    {0xc1f140, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c}},
    {0xc1e2d0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
};
/* Constants the class reads from .rdata: 1.0, the y offset (60.0), the fade
 * limit (15.0) and the five sprite scales. */
static const struct { uint32_t rva; uint32_t bits; } g_hud_row3_const_sig[] = {
    {0x1588938, 0x3f800000u}, {0x12675d8, 0x42700000u}, {0x1268c10, 0x41700000u},
    {0x1244088, 0x3f42d0e5u}, {0x158889c, 0x3f000000u}, {0x12390e0, 0x3f004189u},
};

/* 1 when every native the rewrite calls has the bytes it was written against. */
static int hud_row3_code_ok(void)
{
    static int verdict = -1;
    if (verdict >= 0)
        return verdict;
    verdict = 1;
    for (size_t i = 0; i < sizeof g_hud_row3_native_sig / sizeof g_hud_row3_native_sig[0]; i++) {
        const uint8_t *const at = g_base + g_hud_row3_native_sig[i].rva;
        if (!readable(at, 8) || memcmp(at, g_hud_row3_native_sig[i].bytes, 8)) {
            LOG("hud row3: executable bytes at %#x are not the supported build; "
                "the third row stays off", g_hud_row3_native_sig[i].rva);
            verdict = 0;
            return 0;
        }
    }
    for (size_t i = 0; i < sizeof g_hud_row3_const_sig / sizeof g_hud_row3_const_sig[0]; i++) {
        const uint8_t *const at = g_base + g_hud_row3_const_sig[i].rva;
        if (!readable(at, 4) || *(const uint32_t *)at != g_hud_row3_const_sig[i].bits) {
            LOG("hud row3: constant at %#x is not the supported build's; the third "
                "row stays off", g_hud_row3_const_sig[i].rva);
            verdict = 0;
            return 0;
        }
    }
    return 1;
}

static void *hud_c_sprite(int i)
{
    return *(void **)(g_hud_row3_block + HUD_ROW3_SPRITES_OFF + (size_t)i * 8);
}

/* Init, 0xc1eba0: five sprites, the element id as their layout key. */
static int hud_row3_init_native(void *row)
{
    uint8_t *const c = g_hud_row3_block;
    void **const vt = *(void ***)row;
    int (*const id_of)(void *) = (int (*)(void *))vt[0xa8 / 8];
    const uint8_t ok = HN(hn_b_p, 0xbf33c0)(row);
    void *s = HN(hn_p_fii, 0xddf270)(0.761f, 0x27761, id_of(row));
    *(void **)((uint8_t *)row + HUD_ROW3_CONTAINER_OFF) = s;
    if (!s)
        return 0;
    *((uint8_t *)s + 0x39) = 1;
    HN(hn_v_p, 0xc1eb10)(c);
    static const struct { int id; float scale; } spr[4] = {
        {0x3fe35, 0.5f}, {0x7fcb9, 0.5f}, {0x51dd7, 0.5f}, {0x15ac4, 0.501f},
    };
    for (int i = 0; i < 4; i++) {
        s = HN(hn_p_fii, 0xddf270)(spr[i].scale, spr[i].id, id_of(row));
        *(void **)(c + HUD_ROW3_SPRITES_OFF + (size_t)i * 8) = s;
        if (!s)
            return 0;
    }
    return ok;
}

/* Release, 0xc1e9c0: the container, then the four block sprites. Once. */
static void hud_row3_release_native(void *row)
{
    if (g_hud_row3_released)
        return;
    g_hud_row3_released = 1;
    void *const cont = *(void **)((uint8_t *)row + HUD_ROW3_CONTAINER_OFF);
    if (cont)
        HN(hn_v_p, 0xddf3b0)(cont);
    for (int i = 0; i < 4; i++) {
        void *const s = hud_c_sprite(i);
        if (s)
            HN(hn_v_p, 0xddf3b0)(s);
        *(void **)(g_hud_row3_block + HUD_ROW3_SPRITES_OFF + (size_t)i * 8) = NULL;
    }
}

/* C owns a separate layout. Its container must follow the bar sprites too:
 * hiding the element flags alone leaves that layout painted over menus. */
static int hud_row3_visibility(void *row, uint8_t on, int second)
{
    hn_v_pb set = HN(hn_v_pb, second ? 0xddc090 : 0xdd9980);
    void *const container = *(void **)((uint8_t *)row + HUD_ROW3_CONTAINER_OFF);
    void *const s48 = hud_c_sprite(0), *const s50 = hud_c_sprite(1);
    void *const s58 = hud_c_sprite(2), *const s60 = hud_c_sprite(3);
    const int bound = *(const uint32_t *)(g_hud_row3_block + HUD_ROW3_STATE_OFF) != 0;
    if (!on || bound) {
        if (container) set(container, on);
        if (s48) set(s48, on);
        if (s50) set(s50, on);
    }
    if (s58) set(s58, 0);
    if (s60) set(s60, 0);
    return ((int (*)(void *, uint8_t))(g_base + (second ? 0xbf3dd0 : 0xbf3af0)))(row, on);
}

static void hud_row_hide(void *row)
{
    hud_row3_visibility(row, 0, 0);
    hud_row3_visibility(row, 0, 1);
}

static int hud_row3_gameplay(void *row)
{
    void *const root = hud_row3_elem_at(hud_row3_elements(), 0);
    if (hud_row_wanted() && hud_row3_mission() &&
        root && readable(root, 0x68) &&
        *(void **)root == g_base + HUD_ROOT_VTABLE_RVA &&
        (*(const uint32_t *)((const uint8_t *)root + HUD_ROW_FLAGS_OFF) &
         HUD_ROW_SHOWN) == HUD_ROW_SHOWN)
        return 1;
    /* The result screen can keep the registered element alive after the
     * native HUD disappears. Hide its sprites before disabling its updates. */
    hud_row_hide(row);
    return 0;
}

/* Frame, 0xc210c0: once the container and the two bar sprites resolve, put the
 * bar sprites in their start pose. Returns the class's own success flag. */
static int hud_row3_frame_native(void *row)
{
    if (!HN(hn_b_p, 0xce2c30)(row))
        return 0;
    void *const cont = *(void **)((uint8_t *)row + HUD_ROW3_CONTAINER_OFF);
    if (!cont)
        return 0;
    if (HN(hn_b_p, 0xdd9400)(cont) && !HN(hn_b_p, 0xdd93e0)(cont)) {
        const int e = ((int (*)(uint32_t))(g_base + 0xdcfb70))(0xf2155);
        ((void (*)(void *, int, int, int, int))(g_base + 0xddc0a0))(cont, e, 0, 0, 0);
        ((void (*)(void *, uint32_t))(g_base + 0xdd95d0))(cont, 0xf2155);
    }
    void *const s48 = hud_c_sprite(0), *const s50 = hud_c_sprite(1);
    if (!s48 || !s50 || !HN(hn_b_p, 0xdd93e0)(cont) ||
        !HN(hn_b_p, 0xdd93e0)(s48) || !HN(hn_b_p, 0xdd93e0)(s50))
        return 0;
    HN(hn_v_setf, 0xddc000)(s48, 0, 0.0f);
    HN(hn_v_setf, 0xddc000)(s48, 8, 0.0f);
    for (int i = 3; i <= 6; i++)
        HN(hn_v_pib, 0xddbf00)(s48, i, 0);
    const int e = ((int (*)(void *, int))(g_base + 0xdd6660))(s48, 1);
    if (e != -1)
        HN(hn_v_text, 0xddadc0)(s48, 0x10, 0, e, 0);
    return 1;
}

/* Update, 0xc1f500, for block C. State 0 (empty) is not handled here: it runs
 * the character assigner, which the drive calls by itself (hud_row3_assign) so
 * that its exclusion test sees rows 1 and 2 at their real addresses. States:
 * 1 bound and fading in, 2 showing, 3 the companion went away, 4 following
 * the companion (placing the name tag on the world position). */
static void hud_row3_update_native(void *row, void *arg)
{
    uint8_t *const c = g_hud_row3_block;
    uint32_t *const state = (uint32_t *)(c + HUD_ROW3_STATE_OFF);
    uint8_t *const mgr = *(uint8_t *const *)(g_base + HUD_ROW3_MODE_MGR_RVA);
    void *const cont = *(void **)((uint8_t *)row + HUD_ROW3_CONTAINER_OFF);
    if (!mgr || !readable(mgr + 0x150, 8) || !cont)
        return;
    hn_v_ppp refresh_a = HN(hn_v_ppp, 0xc20c50), refresh_b = HN(hn_v_ppp, 0xc20a00),
             refresh_c = HN(hn_v_ppp, 0xc20580);
    hn_b_pi done = HN(hn_b_pi, 0xdd93c0);
    void *const s48 = hud_c_sprite(0), *const s50 = hud_c_sprite(1);
    void *ch = *(void **)(c + HUD_ROW3_CHAR_OFF);

    HN(hn_v_p, 0xbf3df0)(row);
    switch (*state) {
    case 1:
        if (!ch) {
            HN(hn_v_pp, 0xc1e680)(row, c);
            break;
        }
        if (done(s48, 0) && done(s50, 0))
            *state = 2;
        refresh_a(row, arg, c);
        refresh_b(row, arg, c);
        refresh_c(row, arg, c);
        break;
    case 2:
        if (!ch) {
            HN(hn_v_pp, 0xc1e680)(row, c);
            break;
        }
        if (*(const int *)(mgr + 0x154) == 1) {
            float *const t = (float *)(c + 0x68);
            if (*(const float *)(g_base + 0x1268c10) > *t)
                *t += *(const float *)((const uint8_t *)arg + 0x18322c);
            else
                HN(hn_v_pp, 0xc1e680)(row, c);
        }
        refresh_a(row, arg, c);
        refresh_b(row, arg, c);
        refresh_c(row, arg, c);
        break;
    case 3:
        if (done(s48, 1)) {
            if (!done(s50, 1))
                break;
            HN(hn_v_p, 0xddcb00)(s50);
            if (ch && *((const uint8_t *)ch + 0x430)) {
                const uint8_t *const data = *(const uint8_t *const *)((const uint8_t *)ch + 0x240);
                if (!data)
                    break;
                if (!HN(hn_b_p, 0x7b4290)((void *)(data + 0x10))) {
                    *state = 4;
                    break;
                }
            }
            HN(hn_v_p, 0xc1e310)(c);
        } else {
            if (!ch || !*((const uint8_t *)ch + 0x430))
                break;
            refresh_a(row, arg, c);
            refresh_b(row, arg, c);
        }
        break;
    case 4: {
        if (!ch) {
            HN(hn_v_p, 0xc1e310)(c);
            HN(hn_v_pb, 0xddc090)(cont, 0);
            HN(hn_v_pb, 0xdd9980)(cont, 0);
            break;
        }
        const uint8_t *const data = *(const uint8_t *const *)((const uint8_t *)ch + 0x240);
        if (!data)
            break;
        if (*(const uint64_t *)(data + 0x20) != *(const uint64_t *)(c + 0x08) ||
            *(const uint64_t *)(data + 0x18) != *(const uint64_t *)(c + 0x10)) {
            ((void (*)(void *, void *, void *))(g_base + 0xc1e400))(row, c, ch);
            refresh_a(row, arg, c);
            refresh_b(row, arg, c);
        }
        if (*(const int *)(mgr + 0x154) == 0) {
            ((void (*)(void *, void *, void *))(g_base + 0xc1e400))(row, c,
                                                                    *(void **)(c + HUD_ROW3_CHAR_OFF));
            refresh_a(row, arg, c);
            refresh_b(row, arg, c);
        }
        if (*(const int *)(mgr + 0x150) != 1)
            break;
        float v[4] __attribute__((aligned(16))) = {0, 0, 0, 0};
        ((void (*)(void *, void *))(g_base + 0x765b60))(ch, v);
        v[1] += *(const float *)(g_base + 0x12675d8);
        const uint8_t flag = HN(hn_b_p, 0xc01980)(v);
        HN(hn_v_pib, 0xddbf00)(s48, 0x15, flag);
        uint8_t pose[16] __attribute__((aligned(16)));
        memcpy(pose, ((const void *(*)(void))(g_base + 0x752640))(), sizeof pose);
        ((void (*)(void *, void *))(g_base + 0xc04270))(pose, v);
        HN(hn_v_ppp, 0xdda790)(s48, (void *)0, pose);
        break;
    }
    default:
        break;
    }

    /* Common tail: two steps, then the shown name id follows the character. */
    HN(hn_v_ppp, 0xc20300)(row, arg, c);
    HN(hn_v_ppp, 0xc1fe60)(row, arg, c);
    ch = *(void **)(c + HUD_ROW3_CHAR_OFF);
    const uint8_t *const data = ch ? *(const uint8_t *const *)((const uint8_t *)ch + 0x240) : NULL;
    if (data) {
        void *const names = (void *)(data + 0x9d8);
        int name = *(const int *)((const uint8_t *(*)(void *, int))(g_base + 0x7b2c00))(names, 2);
        const uint8_t *const x = *(const uint8_t *const *)((const uint8_t *)ch + 0x320);
        if (x) {
            const unsigned w = (unsigned)(*(const uint16_t *)(x + 6) & 6u);
            if (w == 2)
                name = 0;
            else if (w == 0)
                name = ((int (*)(void *, int))(g_base + 0x7b2580))(names, 2);
        }
        int *const shown = (int *)(c + 0x70);
        if (name != *shown) {
            int r = 0, ok = 1;
            if (name) {
                const uint8_t *const root = *(const uint8_t *const *)(g_base + 0x1871720);
                const uint8_t *const tbl = root && readable(root + 0x70, 8)
                    ? *(const uint8_t *const *)(root + 0x70) : NULL;
                const uint8_t *const entry = tbl && readable(tbl + 0x428, 8)
                    ? ((const uint8_t *(*)(void *, int))(g_base + 0x77f9b0))(
                          *(void *const *)(tbl + 0x428), name) : NULL;
                if (entry && readable(entry, 0x10))
                    r = ((int (*)(void *, uint32_t, const void *))(g_base + 0xdd6700))(
                        cont, 0xf2155, entry + *(const uint32_t *)(entry + 0xc));
                else
                    ok = 0;     /* the text tables are not up yet: try next frame */
            } else {
                r = ((int (*)(void *, uint32_t, uint32_t))(g_base + 0xdd66c0))(cont, 0xf2155, 0x21);
            }
            if (ok) {
                HN(hn_v_text, 0xddadc0)(cont, 8, 0, r, 0);
                HN(hn_v_anim, 0xdd94c0)(cont, 5, 0, 1.0f, 1);
                *shown = name;
            }
        }
    }

    /* Mode flags of the HUD manager: the name tag follows mode 1. */
    uint8_t *const flag = c + 0x74;
    int mode = *(const int *)(mgr + 0x150);
    if (mode == 1 && !*flag) {
        HN(hn_v_pib, 0xddbf00)(s48, 0x15, 1);
        HN(hn_v_anim, 0xdd94c0)(s48, 8, 0, 1.0f, 1);
        mode = *(const int *)(mgr + 0x150);
    }
    if (mode != 1) {
        if (*flag) {
            HN(hn_v_anim, 0xdd94c0)(s48, 9, 0, 1.0f, 1);
            mode = *(const int *)(mgr + 0x150);
        }
        if (mode != 1 && !*flag && !HN(hn_b_pi, 0xdd9480)(s48, 9))
            HN(hn_v_pib, 0xddbf00)(s48, 0x15, 0);
    }
    *flag = *(const int *)(mgr + 0x150) == 1;
    if (*(const int *)(*(uint8_t *const *)(g_base + HUD_ROW3_MODE_MGR_RVA) + 0x154) == 2) {
        HN(hn_v_pib, 0xddbf00)(s48, 2, 0);
        HN(hn_v_pib, 0xddbf00)(s50, 0, 0);
    }
}

/* A call into our element's own vtable from the game must obey the same
 * freshness rule as the drive: after a HUD rebuild the element and its sprites
 * belong to the previous gauge generation and the class must not run. */
static int hud_row3_self_stale(void *self)
{
    return self == g_hud_row3_elem && g_hud_row3_gen != hud_row3_generation();
}
/* Every failure path after the block was handed to a new element has to drop
 * the active flag: with it left at 1 the drive and the thunks keep treating
 * block C as live while no element is registered. The element is not released:
 * the proxy knows no deleter for the menu's children, and inventing a vtable
 * call into game code is worse than at most one leaked 0x68-byte element per
 * attempt (capped at three). */
static void hud_row3_abandon(void)
{
    InterlockedExchange(&g_hud_row3_active, 0);
    g_hud_row3_elem = NULL;
    g_hud_row3_gen = -1;
    hud_row3_forget();
}

/* The slots of the private vtable. Everything that must not run on a dead or
 * stale element returns before it touches a sprite. */
static void *thunk_dtor(void *self, unsigned flags)
{
    if (self == g_hud_row3_elem) {
        void *const r = ((hud_row_dtor_fn)g_hud_row3_orig[HUD_ROW3_DTOR])(self, flags);
        /* The object and its sprites are gone (the base destructor calls the
         * release slot below): unlink block C's node now, and let the drive
         * clear the registry slot if the game did not. */
        InterlockedExchange(&g_hud_row3_active, 0);
        InterlockedExchange(&g_hud_row3_dead, 1);
        hud_row3_forget();
        LOG("hud row3: the game destroyed the third row, it will be recreated");
        return r;
    }
    return ((hud_row_dtor_fn)g_hud_row3_orig[HUD_ROW3_DTOR])(self, flags);
}

static int thunk_init(void *self)
{
    return g_hud_row3_active && self == g_hud_row3_elem ? hud_row3_init_native(self) : 0;
}

static void thunk_release(void *self)
{
    if (g_hud_row3_active && self == g_hud_row3_elem)
        hud_row3_release_native(self);
}

/* Block A's +0x20 word is written by native class code whenever it hides the
 * row its element id selects; for our id 0x73 that is row 1's block A. Every
 * native call that can trigger the write borrows the word first: a changed
 * value afterwards belongs to C (steal) or is simply put back (restore). */
static uint32_t hud_a20_borrow(void)
{
    return *(uint32_t *)(hud_row3_a() + 0x20);
}

static void hud_a20_return(uint32_t keep, int steal)
{
    uint32_t *const a20 = (uint32_t *)(hud_row3_a() + 0x20);
    if (*a20 == keep)
        return;
    if (steal)
        *(uint32_t *)(g_hud_row3_block + 0x20) = *a20;
    *a20 = keep;
}

/* Slot 0x18 takes (this, per-frame arg, r8); r8 is dead register content the
 * update never reads. Only the drive calls it, once the sprites resolved; the
 * assigner is the drive's job, so an empty block is not updated. */
static int hud_row3_unready(void *row3);
static int thunk_update(void *self, void *arg, void *block)
{
    (void)block;
    if (!g_hud_row3_active || self != g_hud_row3_elem || hud_row3_self_stale(self))
        return 0;
    if (!hud_row3_gameplay(self) || !arg ||
        *(const uint32_t *)(g_hud_row3_block + HUD_ROW3_STATE_OFF) == 0 ||
        hud_row3_unready(self) >= 0)
        return 0;
    /* When the class hides a row it zeroes the +0x20 field of the static block
     * its id selects (the tail of 0xc1fb20), which for id 0x73 is row 1's.
     * Undo that write and give it to C, where the real code would have put it. */
    const uint32_t keep = hud_a20_borrow();
    hud_row3_update_native(self, arg);
    hud_a20_return(keep, 1);
    return 1;
}

static int thunk_frame(void *self)
{
    if (!g_hud_row3_active || self != g_hud_row3_elem || hud_row3_self_stale(self) ||
        !hud_row3_gameplay(self))
        return 0;
    const int r = hud_row3_frame_native(self);
    /* The game itself calls the registered element's frame slot on the game
     * thread; placement and showing are done by the drive, after the update. */
    if (!InterlockedExchange(&g_hud_row3_frame_fired, 1))
        LOG("hud row3: the game drives the frame slot on the game thread");
    return r;
}

static int thunk_b8(void *self, unsigned char on)
{
    if (!g_hud_row3_active || self != g_hud_row3_elem)
        return 0;
    return hud_row3_visibility(self, on, 0);
}

static int thunk_d8(void *self, unsigned char on)
{
    if (!g_hud_row3_active || self != g_hud_row3_elem)
        return 0;
    return hud_row3_visibility(self, on, 1);
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
    if (!hud_row3_code_ok()) {
        g_hud_row3_failed = 1;
        return NULL;
    }
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

    if (!hud_row3_forget())                 /* a clean C, no stale node */
        return NULL;
    InterlockedExchange(&g_hud_row3_active, 1);
    /* What the class constructor 0xc1e230 does, minus its reset of the static
     * block its id selects (that would be row 1's): the base constructor, the
     * class fields, and then init on block C. */
    ((void *(*)(void *, int, int))(g_base + 0xbf30a0))(elem, HUD_ROW3_ID, 0);
    *(void **)elem = g_hud_row3_vtable;     /* private vtable, id getter original */
    *(void **)((uint8_t *)elem + HUD_ROW3_CONTAINER_OFF) = NULL;
    const int inited = hud_row3_init_native(elem);
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
    /* Visible only once a companion is bound and its sprites are resolved: the
     * drive does that, so an empty third row never appears on screen. */
    return elem;
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
    static int since_write, logged;
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
    int changed = !g_hud_row3_have_pos;
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
    g_hud_row3_have_pos = 1;
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


/* The game's own readiness predicate for a sprite (0x140dd93e0): the sprite
 * has a layout link, the link has its container, and the container is not
 * mid-update. The sprite API behind the class update dereferences that
 * container without a check, which is exactly the fault at exe+0x532647
 * (rcx = [[sprite+0x28]+0x28] = 0, access 0x1c8). The class's own frame slot
 * (0xc210c0) guards on this predicate before it initialises anything; the
 * update slot does not, so the update may only run once it holds. */
typedef uint8_t (*hud_sprite_ready_fn)(void *sprite);

static int hud_row3_sprite_ready(void *sprite)
{
    if (!sprite || !readable(sprite, 0x30))
        return 0;
    const uint8_t *const link = *(const uint8_t *const *)((const uint8_t *)sprite + 0x28);
    if (!link || !readable(link, 0x30) || !*(void *const *)(link + 0x28))
        return 0;
    return ((hud_sprite_ready_fn)(g_base + HUD_ROW3_SPRITE_READY_RVA))(sprite) != 0;
}

/* -1 when the container sprite and all four block sprites are ready, else the
 * index of the first one that is not (0 = container, 1..4 = block +0x48..+0x60). */
static int hud_row3_unready(void *row3)
{
    if (!hud_row3_sprite_ready(*(void **)((uint8_t *)row3 + HUD_ROW3_CONTAINER_OFF)))
        return 0;
    for (int i = 0; i < 4; i++)
        if (!hud_row3_sprite_ready(*(void **)(g_hud_row3_block + HUD_ROW3_SPRITES_OFF +
                                              (size_t)i * 8)))
            return i + 1;
    return -1;
}

/* Let the class bind block C to a companion. This is the game's own assigner
 * (0x140c21240) called with block C as the explicit block argument: its
 * exclusion test reads the static nodes of rows 1 and 2 at
 * their real addresses, so it picks a companion neither shows; the character
 * is bound through the reference node inside C itself (0x140c1e400), so the
 * character's release list holds C and clears it when the character dies.
 * That is what the old raw pointer store lacked. One quirk is undone: when
 * the class hides a row it zeroes the +0x20 field of the static block its id
 * selects, which for id 0x73 is row 1's. */
static void hud_row3_assign(void *row3, void *arg)
{
    const uint32_t keep = hud_a20_borrow();
    ((hud_assign_fn)(g_base + HUD_ROW3_ASSIGN_RVA))(row3, arg, g_hud_row3_block);
    hud_a20_return(keep, 1);
}

/* Unbind C natively (0x140c1e310: unlink the reference node, hide the sprites,
 * state 0). Only for a block whose sprites are alive. */
static void hud_row3_unbind(void)
{
    const uint32_t keep = hud_a20_borrow();
    ((hud_unbind_fn)(g_base + HUD_ROW3_RESET_RVA))(g_hud_row3_block);
    hud_a20_return(keep, 0);
}

static void hud_row3_drop(void **registry, const char *why)
{
    uint8_t *const slot = (uint8_t *)registry + (size_t)HUD_ROW3_ID * 8;
    int cleared = 0;
    if (registry && readable(slot, sizeof(void *)) && g_hud_row3_elem &&
        *(void **)slot == g_hud_row3_elem) {
        *(void **)slot = NULL;
        cleared = 1;
    }
    LOG("hud row3: dropped the element (%s, gauge generation %d, registry slot %s)",
        why, g_hud_row3_gen, cleared ? "cleared" : "not ours");
    hud_row3_abandon();
    InterlockedExchange(&g_hud_row3_dead, 0);
    g_hud_row3_settle = 0;
}

/* One game-thread tick for row 3, taken from a native row's own update call.
 * arg is the per-frame context the game passed to that row's update slot; the
 * same context is what our element's update needs. Every transition of block C
 * (build, bind, unbind, update, drop) happens here or in the element's own
 * vtable thunks, i.e. on the game thread; no other thread touches the HUD. */
static void hud_row3_drive(void *arg, void *r8)
{
    if (!g_cfg.hud_row3 || g_cfg.max_players < 4 || g_hud_row3_failed ||
        !g_hud_row3_native_ok)
        return;
    if (!hud_row_wanted() || !hud_row3_mission()) {
        if (g_hud_row3_active && g_hud_row3_elem &&
            !hud_row3_self_stale(g_hud_row3_elem) &&
            hud_row3_elem_at(hud_row3_elements(), HUD_ROW3_ID) == g_hud_row3_elem)
            hud_row3_gameplay(g_hud_row3_elem);
        return;
    }
    if (!arg)
        return;
    const DWORD now = GetTickCount();
    if (now == g_hud_row3_last_drive)
        return;                     /* this frame's tick already ran */
    g_hud_row3_last_drive = now;
    hud_row3_veh_install();

    void **const registry = hud_row3_elements();
    if (!registry)
        return;
    void *const row1 = hud_row3_elem_at(registry, 0x74);
    void *const row2 = hud_row3_elem_at(registry, 0x75);
    if (!hud_row3_native(row1) || !hud_row3_native(row2)) {
        g_hud_row3_settle = 0;
        return;
    }
    void *const root = hud_row3_elem_at(registry, 0);
    if (!root || !readable(root, 0x68) ||
        *(void **)root != g_base + HUD_ROOT_VTABLE_RVA)
        return;
    if ((*(const uint32_t *)((const uint8_t *)root + HUD_ROW_FLAGS_OFF) &
         HUD_ROW_SHOWN) != HUD_ROW_SHOWN) {
        hud_row3_follow_hide(root, 0);
        return;
    }
    void *const found = hud_row3_elem_at(registry, HUD_ROW3_ID);
    const int gen = hud_row3_generation();

    if (g_hud_row3_elem) {
        const char *why = NULL;
        if (g_hud_row3_dead)
            why = "the game destroyed it";
        else if (g_hud_row3_gen != gen)
            why = "the HUD was rebuilt under it";
        else if (found != g_hud_row3_elem)
            why = "its registry slot changed";
        if (why) {
            hud_row3_drop(registry, why);
            return;
        }
    } else {
        if (found) {
            /* A previous screen's element may still be on its way out, so
             * wait out the same settle the build waits for before calling the
             * slot taken. Replacing someone else's element would break it. */
            if (++g_hud_row3_settle >= HUD_ROW3_SETTLE) {
                LOG("hud row3: element slot %#x already holds %p (not ours), "
                    "skipping the third row this session", HUD_ROW3_ID, found);
                g_hud_row3_failed = 1;
            }
            return;
        }
        /* Build only on a settled HUD: both native rows must have their
         * sprites resolved for HUD_ROW3_SETTLE consecutive frames. */
        if (!hud_row3_sprite_ready(*(void **)((uint8_t *)row1 + HUD_ROW3_CONTAINER_OFF)) ||
            !hud_row3_sprite_ready(*(void **)((uint8_t *)row2 + HUD_ROW3_CONTAINER_OFF))) {
            g_hud_row3_settle = 0;
            return;
        }
        if (++g_hud_row3_settle < HUD_ROW3_SETTLE)
            return;
        if (g_hud_row3_attempts >= 3) {
            LOG("hud row3: giving up after %d build attempts", g_hud_row3_attempts);
            g_hud_row3_failed = 1;
            return;
        }
        g_hud_row3_attempts++;
        LOG("hud row3: element slot %#x is free, creating the third row on the "
            "game thread", HUD_ROW3_ID);
        if (hud_row3_build(registry))
            g_hud_row3_wait_frames = 0;
        return;
    }

    void *const row3 = g_hud_row3_elem;
    /* Nothing may touch the new element's sprites before the layout behind them
     * has resolved. The wait is logged so a sprite that never resolves names
     * itself instead of leaving a silent hidden row. */
    const int unready = hud_row3_unready(row3);
    if (unready >= 0) {
        g_hud_row3_wait_frames++;
        if (g_hud_row3_wait_frames == 1 || g_hud_row3_wait_frames % 600 == 0)
            LOG("hud row3: waiting for sprite %d to resolve (%d frames so far, "
                "container %p, block sprites %p %p %p %p)", unready,
                g_hud_row3_wait_frames,
                *(void **)((uint8_t *)row3 + HUD_ROW3_CONTAINER_OFF),
                *(void **)(g_hud_row3_block + 0x48), *(void **)(g_hud_row3_block + 0x50),
                *(void **)(g_hud_row3_block + 0x58), *(void **)(g_hud_row3_block + 0x60));
        return;
    }
    if (g_hud_row3_wait_frames >= 0) {
        LOG("hud row3: all five sprites resolved after %d frames",
            g_hud_row3_wait_frames);
        g_hud_row3_wait_frames = -1;
    }

    void *const char_a = *(void *const *)(hud_row3_a() + HUD_ROW3_CHAR_OFF);
    void *const char_b = *(void *const *)(hud_row3_b() + HUD_ROW3_CHAR_OFF);
    void *bound = *(void *const *)(g_hud_row3_block + HUD_ROW3_CHAR_OFF);
    if (bound && (bound == char_a || bound == char_b)) {
        /* The native rows rebind on their own schedule and know nothing about
         * row 3: when one lands on our companion the native row keeps it. */
        LOG("hud row3: companion %p was taken by a native row; unbinding row 3",
            bound);
        hud_row3_unbind();
        hud_row_hide(row3);
        return;
    }
    if (!bound || *(const uint32_t *)(g_hud_row3_block + HUD_ROW3_STATE_OFF) == 0) {
        hud_row_hide(row3);
        /* Wait until both native rows hold a companion, so the one left over
         * is really the third; and look for one at most every 100 ms. */
        if (!char_a || !char_b || now - g_hud_row3_last_assign < 100)
            return;
        g_hud_row3_last_assign = now;
        hud_row3_assign(row3, arg);
        g_hud_row3_have_pos = 0;    /* the assigner moved the row to row 1's anchor */
        bound = *(void *const *)(g_hud_row3_block + HUD_ROW3_CHAR_OFF);
        if (!bound)
            return;
        LOG("hud row3: bound companion %p natively (state %u, node prev %p next %p)",
            bound, *(const uint32_t *)(g_hud_row3_block + HUD_ROW3_STATE_OFF),
            *(void *const *)(g_hud_row3_block + 0x38),
            *(void *const *)(g_hud_row3_block + 0x40));
    }

    thunk_update(row3, arg, r8);
    if (!*(void *const *)(g_hud_row3_block + HUD_ROW3_CHAR_OFF)) {
        hud_row_hide(row3);         /* the companion was released */
        return;
    }
    /* Acceptance evidence for the live run: what the row holds next to what the
     * character holds, every 2 s and on any change of either. Read only. */
    {
        static DWORD last_ms;
        static uint64_t last_c[2], last_d[2];
        static void *last_char;
        void *const ch = *(void *const *)(g_hud_row3_block + HUD_ROW3_CHAR_OFF);
        const uint8_t *const data = ch && readable((const uint8_t *)ch + 0x240, 8)
            ? *(const uint8_t *const *)((const uint8_t *)ch + 0x240) : NULL;
        const uint64_t c[2] = {*(const uint64_t *)(g_hud_row3_block + 0x08),
                               *(const uint64_t *)(g_hud_row3_block + 0x10)};
        const uint64_t d[2] = {data && readable(data + 0x20, 8) ? *(const uint64_t *)(data + 0x20) : 0,
                               data && readable(data + 0x18, 8) ? *(const uint64_t *)(data + 0x18) : 0};
        if (now - last_ms >= 2000 || ch != last_char || memcmp(c, last_c, sizeof c) ||
            memcmp(d, last_d, sizeof d)) {
            last_ms = now;
            last_char = ch;
            memcpy(last_c, c, sizeof c);
            memcpy(last_d, d, sizeof d);
            LOG("hud row3: row3 char %p hp %llu/%llu (row block), character hp "
                "%llu/%llu, state %u, shown word %#x", ch, (unsigned long long)c[0],
                (unsigned long long)c[1], (unsigned long long)d[0],
                (unsigned long long)d[1],
                *(const uint32_t *)(g_hud_row3_block + HUD_ROW3_STATE_OFF),
                *(const uint32_t *)((const uint8_t *)row3 + HUD_ROW_FLAGS_OFF));
        }
    }
    hud_row3_place(row3, row1, row2);
    if ((*(const uint32_t *)((const uint8_t *)row3 + HUD_ROW_FLAGS_OFF) &
         HUD_ROW_SHOWN) != HUD_ROW_SHOWN) {
        hud_row3_visibility(row3, 1, 0);
        hud_row3_visibility(row3, 1, 1);
    }
    hud_row_show(row3);
    if (!InterlockedExchange(&g_hud_row3_driven, 1))
        LOG("hud row3: per-frame drive confirmed through a native row, "
            "state word %#x, container sprite %p",
            *(const uint32_t *)((const uint8_t *)row3 + HUD_ROW_FLAGS_OFF),
            *(void *const *)((const uint8_t *)row3 + HUD_ROW3_CONTAINER_OFF));
}

/* Piggyback thunks for the *static* vtable: they are entered for rows 1/2 on
 * the game thread and turn the game's own per-frame calls into our ticks. Our
 * element uses the private copy, so this never re-enters itself. The second
 * companion row is made visible here too, on the same thread as everything
 * else the class does to it. */
static void hud_row_keep_shown(void *self)
{
    if (g_cfg.max_players < 3 || !hud_row3_native(self) || !hud_row_wanted())
        return;
    /* Hidden rows do not receive update calls. Row 1 must make the current
     * registry's row 2 eligible before the game reaches it in the same loop. */
    void *const second = hud_row3_elem_at(hud_row3_elements(), 0x75);
    if (hud_row3_native(second))
        hud_row_show(second);
}

static int piggy_update(void *self, void *arg, void *block)
{
    const int r = ((hud_row_arg_fn)g_hud_row3_native_orig[0])(self, arg, block);
    if (!InterlockedExchange(&g_hud_row3_seen_update, 1))
        LOG("hud row3: piggyback: native update slot (0x18) runs per frame "
            "(self %p, arg %p, r8 %p)", self, arg, block);
    hud_row_keep_shown(self);
    hud_row3_drive(arg, block);
    return r;
}

static int piggy_frame(void *self)
{
    const int r = ((hud_row_bool_fn)g_hud_row3_native_orig[1])(self);
    if (!InterlockedExchange(&g_hud_row3_seen_frame, 1))
        LOG("hud row3: piggyback: native frame slot (0x50) runs per frame "
            "(self %p)", self);
    hud_row_keep_shown(self);
    return r;
}

/* Rows 1/2 belong to the main HUD's layout, but C owns a separate layout.
 * Follow the main HUD, not the row flags: those stay on in native menus. */
static void hud_row3_follow_hide(void *self, uint8_t on)
{
    if (on || !g_cfg.hud_row3 || !g_hud_row3_active || !g_hud_row3_elem ||
        !self || !readable(self, 0x68) ||
        *(void **)self != g_base + HUD_ROOT_VTABLE_RVA ||
        hud_row3_self_stale(g_hud_row3_elem))
        return;
    void **const registry = hud_row3_elements();
    if (hud_row3_elem_at(registry, 0) != self ||
        hud_row3_elem_at(registry, HUD_ROW3_ID) != g_hud_row3_elem)
        return;
    hud_row_hide(g_hud_row3_elem);
}

static int piggy_b8(void *self, uint8_t on)
{
    const int r = ((int (*)(void *, uint8_t))g_hud_row3_native_orig[2])(self, on);
    hud_row3_follow_hide(self, on);
    return r;
}

static int piggy_d8(void *self, uint8_t on)
{
    const int r = ((int (*)(void *, uint8_t))g_hud_row3_native_orig[3])(self, on);
    hud_row3_follow_hide(self, on);
    return r;
}

/* Install row update hooks and, for C, the main HUD's visibility hooks.
 * Refuse unknown methods and make both tables writable before any patch. */
static int hud_row3_native_hook(void)
{
    void **const vt = (void **)(g_base + HUD_ROW3_VTABLE_RVA);
    void **const root_vt = (void **)(g_base + HUD_ROOT_VTABLE_RVA);
    const int follow_root = g_cfg.hud_row3 && g_cfg.max_players >= 4;
    const int count = follow_root ? 4 : 2;
    if (!readable(vt, HUD_ROW3_VTABLE_SIZE) ||
        (follow_root && !readable(root_vt, HUD_ROOT_VTABLE_SPAN)))
        return 0;
    void **const table[4] = {vt, vt, root_vt, root_vt};
    void *const want[4] = {g_base + HUD_ROW3_NATIVE_UPDATE_RVA,
                           g_base + HUD_ROW3_NATIVE_FRAME_RVA,
                           g_base + 0xc58fc0u, g_base + 0xc59840u};
    void *const detour[4] = {(void *)piggy_update, (void *)piggy_frame,
                             (void *)piggy_b8, (void *)piggy_d8};
    const int slot[4] = {0x18 / 8, 0x50 / 8, 0xb8 / 8, 0xd8 / 8};
    if (g_hud_row3_native_ok) {
        int intact = 1;
        for (int i = 0; i < count; i++)
            if (table[i][slot[i]] != detour[i])
                intact = 0;
        if (intact)
            return 1;
        LOG("hud row3: piggyback was reverted, reinstalling");
        InterlockedExchange(&g_hud_row3_native_ok, 0);
    }
    for (int i = 0; i < count; i++) {
        if (table[i][slot[i]] != want[i]) {
            LOG("hud row3: piggyback refused: slot %#x holds %p, expected %p",
                slot[i] * 8, table[i][slot[i]], want[i]);
            return 0;
        }
    }
    DWORD old, root_old = 0;
    if (!VirtualProtect(vt, HUD_ROW3_VTABLE_SIZE, PAGE_READWRITE, &old)) {
        LOG("hud row3: cannot write the row vtable (err %lu)", GetLastError());
        return 0;
    }
    if (follow_root &&
        !VirtualProtect(root_vt, HUD_ROOT_VTABLE_SPAN, PAGE_READWRITE, &root_old)) {
        const DWORD err = GetLastError();
        VirtualProtect(vt, HUD_ROW3_VTABLE_SIZE, old, &old);
        LOG("hud row3: cannot write the HUD vtable (err %lu)", err);
        return 0;
    }
    for (int i = 0; i < count; i++)
        g_hud_row3_native_orig[i] = want[i];
    for (int i = 0; i < count; i++)
        table[i][slot[i]] = detour[i];
    if (follow_root)
        VirtualProtect(root_vt, HUD_ROOT_VTABLE_SPAN, root_old, &root_old);
    VirtualProtect(vt, HUD_ROW3_VTABLE_SIZE, old, &old);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    LOG("hud row3: piggyback installed on row update/frame; HUD visibility %s",
        follow_root ? "followed" : "unchanged");
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
            /* In-image qwords on the stack, nearest first: with the faulting
             * offset they name the caller chain without a debugger. They are
             * candidates (stale spills included), not an unwound stack. */
            const uint64_t *sp = (const uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
            char chain[256];
            int cn = 0, found = 0;
            for (int w = 0; w < 192 && found < 10 && readable(sp + w, 8); w++) {
                const uintptr_t v = (uintptr_t)sp[w];
                if (v > (uintptr_t)g_base + 0x1000 && v < (uintptr_t)g_base + size &&
                    cn < (int)sizeof chain - 12) {
                    cn += fmt(chain + cn, (int)sizeof chain - cn, " %#x",
                              (unsigned)(v - (uintptr_t)g_base));
                    found++;
                }
            }
            chain[cn < (int)sizeof chain ? cn : (int)sizeof chain - 1] = 0;
            LOG("hud row3: fault stack candidates (exe+):%s", chain);
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

/* The second row used to be kept alive by a worker thread that wrote the row's
 * state word from outside the game thread and kept raw pointers to rows the
 * game later destroyed. Every row action now runs on the game thread, inside
 * the per-frame slots of the native rows (piggy_update / piggy_frame), so no
 * row pointer is stored and nothing touches the HUD from another thread.
 * This hook only counts HUD generations and arms the piggyback. */
void *det_HudRow(void *self, int id, int index)
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
    if (g_cfg.max_players >= 3 && id != 0x74 && row) {
        LOG("game hud: companion row id=%#x built", id);
        hud_row3_native_hook();
    }
    return row;
}
