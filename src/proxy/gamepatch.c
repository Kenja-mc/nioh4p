#include "nioh3p_internal.h"

/* ------------------------------------------------- game-side host capacity */

static void *EOS_CALL game_create_param_set(void *, void *, const void *);
Hook g_game_create_param_hook = {
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

void install_game_capacity_hook(void)
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



void install_game_metadata_guard(void)
{
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
    g_game_metadata_resume = g_base + 0x5ffea2;
    g_game_metadata_next = g_base + 0x5fff63;
    if (!publish_code16(site, expected, patch, "metadata guard"))
        return;
    LOG("game metadata guard: exact self-assignment keeps the existing attribute buffer");
}

/* Host visitor allowance. The session root (0x141790a70) keeps the number of
 * ordinary visitors the host may admit at +0x39c. The admission handler for
 * raw packet 16 rejects a request (raw packet 7, reason 2) unless
 * allowance - admitted ordinary visitors > 0, so the stock value 1 refuses a
 * second visitor even with three native member slots. Every store that opens
 * recruitment with that single-visitor constant gets max_players - 1 instead;
 * consumption, departure and close paths are unchanged. Code is plaintext only
 * after the stub has decrypted .text, so wait for the exact original bytes.
 * Sites install individually: the mission and shrine paths decrypt during
 * ordinary play, the torii-side store and the second setter call only after
 * that menu is used, and one dormant site must not block the others.
 * The setter 0x140d5fb30 rejects arguments >= 3 (cmp $3, jge at 0x140d5fb4a);
 * when three visitors are configured its immediate is raised too, otherwise
 * the setter keeps the stock cap and stays at 2. */
typedef struct {
    uint32_t rva;           /* first byte of the instruction */
    uint8_t len;
    uint8_t imm;            /* offset of the immediate's low byte */
    uint8_t setter;         /* value is an argument to 0x140d5fb30 */
    const char *name;
    uint8_t bytes[10];
} AllowanceSite;

static const AllowanceSite g_allowance_sites[] = {
    /* session root reset: movl $1, 0x39c(%rcx) */
    {0xd7ac0c, 10, 6, 0, "session root reset",
     {0xc7, 0x81, 0x9c, 0x03, 0, 0, 0x01, 0, 0, 0}},
    /* shrine offering opens recruitment: movb $1, 0x39c(%rbx) */
    {0xd7bb77, 7, 6, 0, "shrine offering",
     {0xc6, 0x83, 0x9c, 0x03, 0, 0, 0x01}},
    /* controller reset: movb $1, R+0x39c */
    {0xd7ca03, 7, 6, 0, "controller reset",
     {0xc6, 0x05, 0x02, 0x44, 0xa1, 0x00, 0x01}},
    /* start recruiting: movb $1, R+0x39c */
    {0xd7daa8, 7, 6, 0, "start recruiting",
     {0xc6, 0x05, 0x5d, 0x33, 0xa1, 0x00, 0x01}},
    /* second start-recruiting store (torii-side flow): movb $1, R+0x39c */
    {0xd7de1f, 7, 6, 0, "start recruiting (alt)",
     {0xc6, 0x05, 0xe6, 0x2f, 0xa1, 0x00, 0x01}},
    /* online shrine offering: mov $1, %ecx; call 0x140d5fb30 */
    {0xcf9f36, 10, 1, 1, "shrine offering setter",
     {0xb9, 0x01, 0, 0, 0, 0xe8, 0xf0, 0x5b, 0x06, 0x00}},
    /* second offering (torii-side flow): mov $2, %ecx; call 0x140d5fb30 */
    {0xcfa515, 10, 1, 1, "torii offering setter",
     {0xb9, 0x02, 0, 0, 0, 0xe8, 0x11, 0x56, 0x06, 0x00}},
};

/* setter cap: cmp $3, %ebx; jge skip at 0x140d5fb4a */
static const AllowanceSite g_setter_cap_site = {
    0xd5fb4a, 4, 2, 2, "setter cap", {0x83, 0xfb, 0x03, 0x7d},
};

void install_game_visitor_allowance(void)
{
    if (g_cfg.max_players < 3 || !g_game_create_param_hook.nslots)
        return;
    uint8_t allowance = (uint8_t)(g_cfg.max_players > 4 ? 3 : g_cfg.max_players - 1);
    const unsigned n = sizeof g_allowance_sites / sizeof g_allowance_sites[0];
    /* A new allowance site must fail the build, not overflow this bitmap. */
    _Static_assert(sizeof g_allowance_sites / sizeof g_allowance_sites[0] <= 8,
                   "installed[] must cover every allowance site");
    uint8_t installed[8] = {0};
    unsigned done = 0;
    int cap_done = 0;
    /* VMProtect decrypts .text lazily: poll tightly for the first minute (the
     * mission start is the interesting window), then slowly for nine more. */
    for (int waited = 0; waited < 600000 && done < n; ) {
        for (unsigned i = 0; i < n; i++) {
            const AllowanceSite *s;
            uint8_t *imm, value;
            DWORD old;
            if (installed[i])
                continue;
            s = &g_allowance_sites[i];
            if (!readable(g_base + s->rva, s->len) ||
                memcmp(g_base + s->rva, s->bytes, s->len))
                continue;
            /* With three visitors configured, hold setter-argument sites
             * until the cap is raised: the stock cap rejects anything >= 3,
             * and a patched site stores once, so installing it first would
             * silently keep the stock 2 for the process lifetime. */
            if (s->setter && allowance > 2 && !cap_done)
                continue;
            /* The page must stay executable while the immediate is stored:
             * another game thread can execute anywhere in this 4 KB page, and
             * a non-executable mapping would fault (DEP). RWX only for the
             * store and the flush, then RX again. */
            imm = g_base + s->rva + s->imm;
            value = allowance;
            if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
                LOG("game visitor allowance: cannot write %#x (err %lu)", s->rva, GetLastError());
                continue;
            }
            *imm = value;
            FlushInstructionCache(GetCurrentProcess(), imm, 1);
            if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READ, &old))
                LOG("game visitor allowance: cannot restore execute protection at %#x (err %lu)",
                    s->rva, GetLastError());
            installed[i] = 1;
            done++;
            LOG("game visitor allowance: %s opens %u visitor slot(s)",
                s->name, value);
        }
        /* Raise the setter's rejection threshold to allowance + 1 before the
         * setter call sites carry more than the stock 2. */
        if (!cap_done && allowance > 2 &&
            readable(g_base + g_setter_cap_site.rva, g_setter_cap_site.len) &&
            !memcmp(g_base + g_setter_cap_site.rva, g_setter_cap_site.bytes,
                    g_setter_cap_site.len)) {
            uint8_t *imm = g_base + g_setter_cap_site.rva + g_setter_cap_site.imm;
            DWORD old;
            if (VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
                *imm = (uint8_t)(allowance + 1);
                FlushInstructionCache(GetCurrentProcess(), imm, 1);
                if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READ, &old))
                    LOG("game visitor allowance: cannot restore execute protection at %#x (err %lu)",
                        g_setter_cap_site.rva, GetLastError());
                cap_done = 1;
                LOG("game visitor allowance: setter cap raised to %u", allowance + 1);
            } else {
                LOG("game visitor allowance: cannot write setter cap %#x (err %lu)",
                    g_setter_cap_site.rva, GetLastError());
            }
        }
        if (done < n) {
            int step = waited < 60000 ? 10 : 250;
            Sleep((DWORD)step);
            waited += step;
        }
    }
    LOG("game visitor allowance: recruitment opens %u visitor slot(s); %u/%u site(s) patched%s",
        allowance, done, n, cap_done ? "; setter cap raised" : "");
}

/* --------------------------------- Torii Gate companion (Tokoyo Coop) for 3-4 */

/* Yokai Realm with a Companion is written for two players. Two places keep a
 * second companion (member slot 2 or 3) out of the mission:
 *
 * 1. Preparation. The host sends the mission preview (packet 76), its ready
 *    flag (74), the start flag (75) and the chosen mission (17) to slot 1
 *    only, so a later companion waits on "Waiting for mission to be chosen".
 *    The send hook mirrors these four packets to slots 2 and 3. Packet 17
 *    carries the receiver's own slot at payload byte 6 (the visitor's handler
 *    adopts it), so each copy gets its own slot. The send byte-swaps its
 *    buffer in place, so the copies come from a snapshot taken before.
 * 2. Mission start. Online::AddFlagWaitCoopPlayer (0x1408af1b0) is the
 *    mission script's sync point: the host bumps mission flag 0x46, a visitor
 *    bumps 0x47, and Online::IsWaitCoopPlayer holds each side until the other
 *    catches up. 0x47 is one shared counter, so with two companions it runs
 *    ahead of the host (host 1, visitors 3) and every client waits forever on
 *    the loading screen. Only the slot-1 companion advances it; later
 *    companions follow the host and slot 1.
 *
 * Game protocol entry points (docs/RE_TOKOYO_COOP.md):
 *   0x140d6a560 send(id, data, len <= 0x100, target): target 0-3 is one
 *               member slot, 4 or more is every connected slot but our own.
 *   0x140d6acc0 dispatch(record, sender), research trace only: record+0x10
 *               is the u16 raw id, record+0x18 the network-order payload.
 * The hooks replace whole prologue instructions with a 14-byte absolute jump.
 * They install at boot, before any session exists, so no game thread runs
 * these functions while the bytes change. */

uint8_t *game_manager_ptr(void)
{
    uint8_t *const *slot = (uint8_t *const *)(g_base + GAME_MANAGER_RVA);
    if (!readable(slot, sizeof(void *)))
        return 0;
    uint8_t *m = *slot;
    return (m && readable(m, 0x5b4)) ? m : 0;
}

/* -------------------------------------------------- game-side friend invite */

/* A stock invite can never join. The GameLobbyJoinRequested callback handler
 * (0x1405ef7e0) stores the lobby id and requests lobby data, but the pump
 * turns the "lobby owner is someone else" state into error 0x80C81010 and
 * latches the error bits that block every later session open. Nothing ever
 * reaches SessionOpen_Start. Two small changes give an invite the same join
 * the Torii Gate search uses:
 *   1. the pump's state-3 branch ("cmp $0x3,%eax; je" at 0x5ef001) is
 *      retargeted to the state-2 path, which only clears the invite state;
 *   2. the callback's Run slot (vtable 0x141586b60, entry 1) is hooked. After
 *      the original Run, the invited lobby id goes to the game's own
 *      join-by-lobby task 0x140d73380(task, ptr_to_lobby_id), which builds
 *      the kind-1 join descriptor and calls SessionOpen_Start. The
 *      descriptor's second field is a pointer to the lobby id; the join
 *      parameter's SetParam (0x1405f1390) dereferences it exactly once.
 *      The task object is the open-request singleton at [manager+0xA8]. It
 *      only exists once the game's session steps have created it; an idle
 *      client has none (both game call sites read it straight and the game's
 *      step 0x140d65740 creates it through factory 0x140d66910). The same
 *      factory runs here first, so an invite works from a fresh boot. */
static uint64_t g_invite_lobby_storage;
#ifdef NIOH3P_RESEARCH
static int g_invite_installed;  /* gates the rig trigger below */
#endif

static void game_invite_join(uint64_t lobby)
{
    static const uint8_t join_prologue[] = {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x40,
    };
    static const uint8_t factory_prologue[] = {
        0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24,
    };
    uint8_t *join = g_base + 0xd73380;
    if (!readable(join, sizeof join_prologue) ||
        memcmp(join, join_prologue, sizeof join_prologue)) {
        LOG("game invite: join task instructions mismatch; invite dropped");
        return;
    }
    void **manager_slot = (void **)(g_base + 0x17900b0);
    if (!readable(manager_slot, sizeof *manager_slot) || !*manager_slot) {
        LOG("game invite: game manager missing; invite dropped");
        return;
    }
    void *manager = *manager_slot;
    if (!readable(manager, 0xb0)) {
        LOG("game invite: manager unreadable; invite dropped");
        return;
    }
    void *task = *(void **)((uint8_t *)manager + 0xa8);
    if (!task) {
        uint8_t *factory = g_base + 0xd66910;
        if (!readable(factory, sizeof factory_prologue) ||
            memcmp(factory, factory_prologue, sizeof factory_prologue)) {
            LOG("game invite: open task factory mismatch; invite dropped");
            return;
        }
        typedef void (*OpenTaskFactory)(void *manager);
        ((OpenTaskFactory)(void *)factory)(manager);
        task = *(void **)((uint8_t *)manager + 0xa8);
        if (!task) {
            LOG("game invite: open task creation failed; invite dropped");
            return;
        }
        LOG("game invite: open task object created");
    }
    g_invite_lobby_storage = lobby;
    typedef int (*JoinByLobby)(void *, uint64_t *);
    int posted = ((JoinByLobby)(void *)join)(task, &g_invite_lobby_storage);
    LOG("game invite: lobby %llu %s", (unsigned long long)lobby,
        posted ? "join posted" : "join refused (an open request may already be running)");
}

static void game_invite_run(void *self, void *param, uint8_t failed, uint64_t call);
static Hook g_game_invite_hook = {
    .name = "CCallback<CSessionP2pSteam,GameLobbyJoinRequested_t>::Run",
    .detour = (void *)(uintptr_t)game_invite_run,
};

static void game_invite_run(void *self, void *param, uint8_t failed, uint64_t call)
{
    typedef void (*RunFn)(void *, void *, uint8_t, uint64_t);
    ((RunFn)g_game_invite_hook.orig)(self, param, failed, call);
    /* GameLobbyJoinRequested_t: m_ulSteamIDLobby is the first field. */
    if (param && readable(param, sizeof(uint64_t)))
        game_invite_join(*(uint64_t *)param);
}

void install_game_invite_hooks(void)
{
    /* the capacity hook proves the supported game image; without it (harness,
     * unsupported build) the invite patch has nothing to attach to */
    if (!g_game_create_param_hook.nslots)
        return;
    static const uint8_t pump[] = {0x83, 0xf8, 0x03, 0x74, 0x14};
    void **run_slot = (void **)(g_base + 0x1586b68);
    int ready = 0;
    /* VMProtect decrypts .text lazily: poll tightly for the first minute (an
     * invite can only matter once online play starts), then slowly. */
    for (int waited = 0; waited < 600000 && !ready; ) {
        ready = readable(run_slot - 1, 3 * sizeof(void *)) &&
                executable_ptr(run_slot[-1]) &&
                run_slot[-1] == (void *)(g_base + 0x5f0650) &&
                run_slot[0] == (void *)(g_base + 0x5f07d0) &&
                run_slot[1] == (void *)(g_base + 0x5f0640) &&
                readable(g_base + 0x5ef001, sizeof pump) &&
                !memcmp(g_base + 0x5ef001, pump, sizeof pump);
        if (!ready) {
            int step = waited < 60000 ? 10 : 250;
            Sleep((DWORD)step);
            waited += step;
        }
    }
    if (!ready) {
        LOG("game invite: callback vtable or pump instructions mismatch; not installed");
        return;
    }
    if (!add_slot(&g_game_invite_hook, run_slot)) {
        LOG("game invite: callback slot not hooked; not installed");
        return;
    }
    /* Retarget the state-3 jump to the state-2 clear path: the rel8 operand
     * of the je at 0x5ef004 changes from +0x14 (the error block at 0x5ef01a)
     * to -0x10 (0x5eeff6, which only clears the invite state). Same RWX store
     * window as the allowance sites: the page stays executable for the store
     * and the flush, then returns to RX. */
    uint8_t *imm = g_base + 0x5ef005;
    DWORD old;
    if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
        LOG("game invite: cannot write %#x (err %lu)", 0x5ef005, GetLastError());
        return;
    }
    *imm = 0xf0;
    FlushInstructionCache(GetCurrentProcess(), imm, 1);
    if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READ, &old))
        LOG("game invite: cannot restore execute protection at %#x (err %lu)",
            0x5ef005, GetLastError());
#ifdef NIOH3P_RESEARCH
    g_invite_installed = 1;
#endif
    LOG("game invite: friend invites join directly (callback hooked, error state neutralised)");
}

#ifdef NIOH3P_RESEARCH
/* Rig trigger. The emulator rig cannot deliver GameLobbyJoinRequested_t, so
 * a file named nioh3p.invite.txt in the game directory (the proxy's working
 * directory) holding a decimal lobby id posts the same join. The file is
 * consumed on use, so the run controls when the "invite" arrives: drive the
 * visitor to the world map first, then write the file. Research build only. */
static int g_runcallbacks_ticks;
static int g_fake_invite_seen;

void det_SteamRunCallbacks(void)
{
    orig_RunCallbacks();
    if (++g_runcallbacks_ticks % 120)   /* about once per two seconds */
        return;
    if (!g_invite_installed || g_fake_invite_seen)
        return;
    HANDLE f = CreateFileA("nioh3p.invite.txt", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    char buf[32];
    DWORD got = 0;
    BOOL ok = ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    if (!ok)
        return;
    uint64_t v = 0;
    for (DWORD i = 0; i < got; i++) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ')
            continue;
        if (buf[i] < '0' || buf[i] > '9')
            return;             /* not a lobby id; leave the file alone */
        v = v * 10 + (uint64_t)(buf[i] - '0');
    }
    if (!v)
        return;
    g_fake_invite_seen = 1;
    MoveFileA("nioh3p.invite.txt", "nioh3p.invite.used.txt");
    LOG("game invite: synthetic invite for lobby %llu", (unsigned long long)v);
    game_invite_join(v);
}
#endif
