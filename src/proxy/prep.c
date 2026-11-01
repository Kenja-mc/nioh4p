#include "nioh3p_internal.h"

/* ------------------------------ preparation roster: native help text ----- */

/* Update 0xd2d900 visits the interactive preparation state every frame.
 * The native description builder also updates its widget (+0x30), so do not
 * call it twice. Cache its localized ready text at 0xd2e341 and refresh only
 * the raw slot-0xf text when membership changes. Labels 0xb..0xe stay native. */
#define PREP_HELP_CALL_RVA 0xd2e341u
#define PREP_SET_TEXT_RVA  0xddaf90u
#define PREP_UTF8_RVA      0xddf4c0u
#define PREP_SESSION_RVA   0x17900b0u
#define PREP_MEMBER_STRIDE 0xd0u
#define PREP_MEMBER_NAME   0x10u

typedef struct {
    const uint8_t *members;
    unsigned capacity, count, self;
} PrepMembers;

typedef uintptr_t (*PrepUpdate)(void *, void *);
PrepUpdate g_prep_update;
static volatile LONG g_prep_ready;
/* Active only inside the live menu update; never retain a destroyed widget. */
static void *g_prep_sprite, *g_prep_rendered_sprite, *g_prep_stock_sprite;
static wchar_t g_prep_stock[256];
static unsigned g_prep_stock_chars, g_prep_capacity, g_prep_self;
static char g_prep_names[4][64];

typedef int (*PrepSetText)(void *, int, const wchar_t *, int);
typedef int (*PrepUtf8)(wchar_t *, int, const char *, int);

static const char *prep_member_name(const PrepMembers *state, unsigned i)
{
    return (const char *)(state->members + i * PREP_MEMBER_STRIDE + PREP_MEMBER_NAME);
}

static int prep_members(PrepMembers *state)
{
    if (g_cfg.max_players < 3)
        return 0;
    const int32_t *mode = (const int32_t *)(g_base + GAME_COOP_MODE_RVA);
    uint8_t *manager = game_manager_ptr();
    if (!readable(mode, 4) || *mode != 1 || !manager ||
        !readable(manager + GAME_LOADER_OFF, 4) ||
        *(const uint32_t *)(manager + GAME_LOADER_OFF) >= GAME_LOADER_IN_MISSION)
        return 0;
    const uint8_t *session_manager = g_base + PREP_SESSION_RVA;
    if (!readable(session_manager + 0xa8, 8) ||
        !readable(session_manager + 0xc0, 8))
        return 0;
    const uint8_t *session = *(uint8_t *const *)(session_manager + 0xa8);
    if (!session || !readable(session + 0xc0, 4))
        return 0;
    state->capacity = *(const uint32_t *)(session + 0xc0);
    state->members = *(uint8_t *const *)(session_manager + 0xc0);
    const uint8_t *root = g_base + GAME_ROOT_RVA;
    if (!state->capacity || state->capacity > 4 || !state->members ||
        !readable(state->members, state->capacity * PREP_MEMBER_STRIDE) ||
        !readable(root + 0x3c8, 1) || root[0x3c8] >= state->capacity)
        return 0;
    state->self = root[0x3c8];
    if (!prep_member_name(state, state->self)[0])
        return 0;
    /* Native CSession +0xc0 includes empty slots, not just joined players. */
    state->count = 0;
    for (unsigned i = 0; i < state->capacity; i++)
        if (prep_member_name(state, i)[0])
            state->count++;
    return 1;
}

static int prep_members_unchanged(void *sprite, const PrepMembers *state)
{
    if (sprite != g_prep_rendered_sprite || state->capacity != g_prep_capacity ||
        state->self != g_prep_self)
        return 0;
    for (unsigned i = 0; i < state->capacity; i++) {
        const char *name = prep_member_name(state, i);
        for (unsigned j = 0; j < 63; j++) {
            if (name[j] != g_prep_names[i][j])
                return 0;
            if (!name[j])
                break;
        }
    }
    return 1;
}

static void prep_remember_members(void *sprite, const PrepMembers *state)
{
    g_prep_rendered_sprite = sprite;
    g_prep_capacity = state->capacity;
    g_prep_self = state->self;
    for (unsigned i = 0; i < state->capacity; i++) {
        const char *name = prep_member_name(state, i);
        unsigned j = 0;
        while (j < 63 && name[j]) {
            g_prep_names[i][j] = name[j];
            j++;
        }
        g_prep_names[i][j] = 0;
    }
}

static int det_PrepHelpText(void *sprite, int slot, const wchar_t *text, int chars)
{
    PrepSetText original = (PrepSetText)(g_base + PREP_SET_TEXT_RVA);
    if (g_cfg.max_players < 3 || slot != 0xf || chars < 1 || chars > 256 ||
        !readable(text, (size_t)chars * sizeof *text) || text[chars - 1])
        return original(sprite, slot, text, chars);
    if (text != g_prep_stock)
        memcpy(g_prep_stock, text, (size_t)chars * sizeof *text);
    g_prep_stock_chars = (unsigned)chars;
    g_prep_stock_sprite = sprite;
    PrepMembers state;
    if (sprite != g_prep_sprite || !prep_members(&state)) {
        g_prep_rendered_sprite = NULL;
        return original(sprite, slot, text, chars);
    }
    const wchar_t *chosen = text;
    int chosen_chars = chars;
    /* Includes room for the last 64-unit conversion before its 16-unit trim. */
    wchar_t output[384];
    unsigned shown = 0;
    if (state.count >= 3) {
        unsigned used = (unsigned)chars - 1;
        memcpy(output, text, used * sizeof *text);
        wchar_t prefix[] = L"\n\n0 players: ";
        prefix[2] = (wchar_t)(L'0' + state.count);
        memcpy(output + used, prefix, sizeof prefix - sizeof *prefix);
        used += sizeof prefix / sizeof *prefix - 1;
        PrepUtf8 convert = (PrepUtf8)(g_base + PREP_UTF8_RVA);
        for (unsigned i = 0; i < state.capacity; i++) {
            const char *name = prep_member_name(&state, i);
            if (i == state.self || !name[0])
                continue;
            char clean[64];
            unsigned bytes = 0;
            for (unsigned r = 0; r < 63 && name[r]; r++) {
                unsigned char c = (unsigned char)name[r];
                if (c >= 0x20 && c != 0x7f)
                    clean[bytes++] = (char)c;
            }
            clean[bytes] = 0;
            if (!bytes)
                goto write_text;
            if (shown)
                output[used++] = L'\n';
            wchar_t *destination = output + used;
            int converted = convert(destination, 64, clean, (int)bytes + 1);
            if (converted <= 0)
                goto write_text;
            unsigned length = 0;
            for (int r = 0; r < converted && r < 64 && destination[r]; r++) {
                wchar_t c = destination[r];
                if (c < 0x20 || (c >= 0x7f && c <= 0x9f) ||
                    c == 0x2028 || c == 0x2029)
                    continue;
                destination[length++] = c;
            }
            if (!length)
                goto write_text;
            if (length > 16) {
                length = 13;
                if (destination[length - 1] >= 0xd800 &&
                    destination[length - 1] <= 0xdbff)
                    length--;
                destination[length++] = L'.';
                destination[length++] = L'.';
                destination[length++] = L'.';
            }
            destination[length] = 0;
            used += length;
            shown++;
        }
        output[used] = 0;
        chosen = output;
        chosen_chars = (int)used + 1;
    }
write_text:;
    int result = original(sprite, slot, chosen, chosen_chars);
    if (result) {
        if (chosen == output && !prep_members_unchanged(sprite, &state))
            LOG("prep roster: %u player(s), %u companion names in help slot 0xf",
                state.count, shown);
        prep_remember_members(sprite, &state);
    }
    return result;
}

static void *prep_live_sprite(void *self)
{
    const uint8_t *menu = self;
    /* Native state 2 owns the ready widget. Other states open or close it. */
    if (g_cfg.max_players < 3 || !readable(menu + 0x60, 0x30) ||
        *(const uint32_t *)(menu + 0x60) != 2)
        return NULL;
    return *(void *const *)(menu + 0x68);
}

uintptr_t det_PrepUpdate(void *self, void *context)
{
    g_prep_sprite = prep_live_sprite(self);
    if (!g_prep_sprite)
        g_prep_rendered_sprite = NULL;
    uintptr_t result = g_prep_update(self, context);
    /* Native update can close the menu or replace its widgets. Re-read them. */
    void *sprite = prep_live_sprite(self);
    g_prep_sprite = sprite;
    PrepMembers state;
    if (!__atomic_load_n(&g_prep_ready, __ATOMIC_ACQUIRE) || !sprite ||
        !prep_members(&state)) {
        g_prep_rendered_sprite = NULL;
        goto done;
    }
    if (prep_members_unchanged(sprite, &state) ||
        sprite != g_prep_stock_sprite || !g_prep_stock_chars)
        goto done;
    void *widget = *(void **)((uint8_t *)self + 0x88);
    if (!widget || !readable(widget, 8))
        goto done;
    void **vtable = *(void ***)widget;
    if (!vtable || !readable(vtable + 0x50 / 8, 8) ||
        !readable(vtable[0x50 / 8], 1))
        goto done;
    typedef int (*Selection)(void *);
    if (((Selection)vtable[0x50 / 8])(widget) == 0)
        det_PrepHelpText(sprite, 0xf, g_prep_stock, (int)g_prep_stock_chars);
done:
    g_prep_sprite = NULL;
    return result;
}

unsigned relocate_prep_update(uint8_t *out)
{
    /* mov r11,rsp; push rbp; push rdi; push r14; lea rbp,[r11-0x108] uses
     * registers only, so the 14-byte prologue replays verbatim. */
    static const uint8_t head[] = {
        0x4c, 0x8b, 0xdc, 0x55, 0x57, 0x41, 0x56,
        0x49, 0x8d, 0xab, 0xf8, 0xfe, 0xff, 0xff,
    };
    memcpy(out, head, sizeof head);
    return sizeof head;
}


int install_prep_roster(void)
{
    static const uint8_t expected[16] = {
        0x00, 0xe8, 0x4a, 0xcc, 0x0a, 0x00, 0x48, 0x8b,
        0x8c, 0x24, 0x30, 0x10, 0x00, 0x00, 0x48, 0x33,
    };
    static const uint8_t setter[] = {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20,
    };
    static const uint8_t converter[] = {
        0x48, 0x83, 0xec, 0x38, 0x89, 0x54, 0x24, 0x28, 0x33, 0xd2,
    };
    uint8_t *window = g_base + PREP_HELP_CALL_RVA - 1;
    if (!readable(window, sizeof expected) ||
        memcmp(window, expected, sizeof expected) ||
        !readable(g_base + PREP_SET_TEXT_RVA, sizeof setter) ||
        memcmp(g_base + PREP_SET_TEXT_RVA, setter, sizeof setter) ||
        !readable(g_base + PREP_UTF8_RVA, sizeof converter) ||
        memcmp(g_base + PREP_UTF8_RVA, converter, sizeof converter))
        return 0;
    uint8_t *call = window + 1;
    uint8_t *thunk = alloc_rel32(call, 16);
    if (!thunk)
        return -1;
    abs_jmp(thunk, (void *)det_PrepHelpText);
    DWORD old;
    if (!VirtualProtect(thunk, 16, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), thunk, 14);
    uint8_t patch[16];
    memcpy(patch, expected, sizeof patch);
    int32_t relative = (int32_t)((int64_t)(uintptr_t)thunk -
                                (int64_t)(uintptr_t)(call + 5));
    memcpy(patch + 2, &relative, sizeof relative);
    if (!publish_code16(window, expected, patch, "preparation help roster")) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        return -1;
    }
    __atomic_store_n(&g_prep_ready, 1, __ATOMIC_RELEASE);
    LOG("prep roster: native help call 0xd2e341 intercepted; roster refresh rides the per-frame update 0xd2d900");
    return 1;
}
