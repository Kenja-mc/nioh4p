#include "nioh3p_internal.h"

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

int pkt_should_log(int dir, unsigned id)
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

typedef Nioh4pHook InlineHook;

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

static InlineHook g_inline_hooks[] = {
    {PREP_UPDATE_RVA, 14, "preparation update", (void *)det_PrepUpdate,
     (void **)&g_prep_update,
     {0x4c, 0x8b, 0xdc, 0x55, 0x57, 0x41, 0x56, 0x49, 0x8d, 0xab, 0xf8, 0xfe,
      0xff, 0xff}, relocate_prep_update},
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

DWORD WINAPI tokoyo_hooks_worker(LPVOID unused)
{
    (void)unused;
    /* Sized from the table: a new companion hook must fail to compile rather
     * than overflow these. */
    enum { HOOKS_MAX = (int)(sizeof g_inline_hooks / sizeof g_inline_hooks[0]) };
    enum { HOOK_TRIES = 4 };
    const unsigned n = (unsigned)HOOKS_MAX;
    unsigned ok = 0;
    int state[HOOKS_MAX] = {INSTALL_WAIT};
    int tries[HOOKS_MAX] = {0};     /* failed attempts so far */
    int roster = 0;
    /* INSTALL_FAIL stays unsettled until HOOK_TRIES: allocation and
     * protection errors can clear once .text is plaintext. */
    for (int waited = 0; waited < 600000; waited += 50) {
        unsigned pending = 0;
        for (int i = 0; i < HOOKS_MAX; i++) {
            if (state[i] == INSTALL_OK ||
                (state[i] == INSTALL_FAIL && tries[i] >= HOOK_TRIES))
                continue;
            state[i] = inline_install(&g_inline_hooks[i]);
            if (state[i] == INSTALL_OK) {
                ok++;
            } else if (state[i] == INSTALL_FAIL && ++tries[i] >= HOOK_TRIES) {
                LOG("game tokoyo: %s hook failed %d time(s), giving up",
                    g_inline_hooks[i].name, tries[i]);
            } else {
                pending++;
            }
        }
        if (!roster) {
            roster = install_prep_roster();
            if (!roster)
                pending++;
            else if (roster < 0)
                LOG("prep roster: call-site installation failed");
        }
        if (!pending)
            break;
        Sleep(50);
    }
    LOG("game tokoyo: %u/%u companion hook(s) installed", ok, n);
    if (!roster)
        LOG("prep roster: native help instructions did not become available");
    return 0;
}
