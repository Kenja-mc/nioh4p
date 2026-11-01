/* Consumer-side HP regression against the actual gameplay DLL's detour.
 * The native recalc writes a fresh max and clamps current at
 * 0x1407c8696..0x1407c86a4. Replaying that contract must not heal a wounded
 * eligible actor when the add-on reapplies its maximum-HP multiplier.
 * The side-field cases use the native player/escort/enemy comparison.
 * They do not replace gameplay proof. */
#include "../../src/common/nioh4p_addon.h"

struct Status {
    uint8_t *character;
    uint64_t reserved[2];
    uint64_t maximum;
    uint64_t current;
};
_Static_assert(offsetof(struct Status, maximum) == 0x18, "native max offset");
_Static_assert(offsetof(struct Status, current) == 0x20, "native current offset");

static struct Status status;
static uint64_t character[(0xe98 + 7) / 8];
static uint32_t character_data[2];
static uint64_t native_maximum;
static uint64_t local_character[(0xb0 + 7) / 8];
static uint64_t party[4 * 3];
static uint32_t actor_side[5];
static uint32_t local_side[5];

static void say(const char *message)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, lstrlenA(message),
              &written, NULL);
}

static void number(uint64_t value)
{
    char buffer[21];
    char *end = buffer + sizeof buffer;
    *--end = 0;
    do {
        *--end = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    say(end);
}

static void addon_log(const char *format, ...)
{
    (void)format;
}

static int inside(const void *address, size_t size, const void *object,
                  size_t span)
{
    const uintptr_t p = (uintptr_t)address;
    const uintptr_t start = (uintptr_t)object;
    return size <= span && p >= start && p - start <= span - size;
}

static int readable(const void *address, size_t size)
{
    return inside(address, size, &status, sizeof status) ||
           inside(address, size, character, sizeof character) ||
           inside(address, size, character_data, sizeof character_data) ||
           inside(address, size, local_character, sizeof local_character) ||
           inside(address, size, party, sizeof party) ||
           inside(address, size, actor_side, sizeof actor_side) ||
           inside(address, size, local_side, sizeof local_side);
}

static Nioh4pCoreApi core = {
    .version = NIOH4P_ADDON_ABI,
    .size = sizeof core,
    .max_players = 4,
    .log = addon_log,
    .readable = readable,
};

/* Same clamp-only HP effect as the verified native instructions. Other
 * character statistics do not participate in this regression. */
static void native_recalc(void *pointer)
{
    struct Status *value = pointer;
    value->maximum = native_maximum;
    if (value->current > value->maximum)
        value->current = value->maximum;
}

enum ActorKind { HOSTILE, FRIENDLY, NO_SIDE, BAD_SIDE, NO_LOCAL_SIDE };

struct Case {
    const char *name;
    int percent;
    uint64_t old_maximum;
    uint64_t old_current;
    uint64_t native_maximum;
    uint64_t expected_maximum;
    uint64_t expected_current;
    enum ActorKind kind;
};

static const struct Case cases[] = {
    {"full-health initialization", 200, 1000, 1000, 1000, 2000, 2000, HOSTILE},
    {"wounded above native max",   200, 2000, 1500, 1000, 2000, 1500, HOSTILE},
    {"wounded at native max",      200, 2000, 1000, 1000, 2000, 1000, HOSTILE},
    {"wounded below native max",   200, 2000,  750, 1000, 2000,  750, HOSTILE},
    {"dead actor stays dead",      200, 2000,    0, 1000, 2000,    0, HOSTILE},
    {"lower multiplier clamps",     50, 1000,  750, 1000,  500,  500, HOSTILE},
    {"lower max preserves wound",   50,  500,  200, 1000,  500,  200, HOSTILE},
    {"native max decrease",        200, 2000, 1500,  800, 1600, 1500, HOSTILE},
    {"native decrease below HP",   200, 2000, 1500,  600, 1200, 1200, HOSTILE},
    {"stock multiplier clamps",    100, 1000,  900,  800,  800,  800, HOSTILE},
    {"larger max does not heal",   300, 2000, 1500, 1000, 3000, 1500, HOSTILE},
    {"friendly escort stays native", 200, 5292, 588, 5292, 5292, 588, FRIENDLY},
    {"missing side stays native",  200, 1000,  750, 1000, 1000,  750, NO_SIDE},
    {"unreadable side stays native", 200, 1000, 750, 1000, 1000, 750, BAD_SIDE},
    {"unready local side stays native", 200, 1000, 750, 1000, 1000, 750, NO_LOCAL_SIDE},
};

void mainCRTStartup(void)
{
    /* The numeric base maps only the owned party records at the native RVA.
     * No full game image or native process memory is allocated. */
    core.game_base = (uintptr_t)party - 0x18a0490u;
    party[0] = (uintptr_t)local_character;
    HMODULE module = LoadLibraryA("nioh4p-enemy-health.dll");
    Nioh4pAddonInitFn init = module ? (Nioh4pAddonInitFn)
        GetProcAddress(module, "Nioh4pAddonInit") : NULL;
    const Nioh4pAddon *addon = init ? init(&core, module) : NULL;
    if (!addon || addon->version != NIOH4P_ADDON_ABI ||
        addon->size < sizeof *addon || addon->id != NIOH4P_ENEMY_HEALTH ||
        !addon->rules_changed || !addon->hooks) {
        say("FAIL: cannot initialize the actual enemy-health DLL\n");
        ExitProcess(2);
    }
    const Nioh4pHook *hook = NULL;
    for (uint32_t i = 0; i < addon->hook_count; ++i)
        if (addon->hooks[i].rva == 0x7c7eb0u)
            hook = &addon->hooks[i];
    if (!hook || !hook->detour || !hook->orig) {
        say("FAIL: status-recalc detour unavailable\n");
        ExitProcess(2);
    }
    *hook->orig = (void *)native_recalc;
    void (*detour)(void *) = (void (*)(void *))hook->detour;
    *(const void **)((uint8_t *)character + 0xe90) = character_data;
    status.character = (uint8_t *)character;

    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const struct Case *test = &cases[i];
        character_data[1] = test->kind == FRIENDLY ? 0x01820038u : 0x58u;
        actor_side[4] = test->kind == FRIENDLY ? 0u : 1u;
        *(const void **)((uint8_t *)character + 0xa8) =
            test->kind == NO_SIDE ? NULL :
            test->kind == BAD_SIDE ? (const void *)(uintptr_t)1 : actor_side;
        *(const void **)((uint8_t *)local_character + 0xa8) =
            test->kind == NO_LOCAL_SIDE ? NULL : local_side;
        Nioh4pRules rules = {test->percent, 100, 0};
        addon->rules_changed(&rules);
        status.maximum = test->old_maximum;
        status.current = test->old_current;
        native_maximum = test->native_maximum;
        int passed = 1;
        /* The second recalc covers the transition from native max to an
         * already scaled max, not just the initial multiplication. */
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            detour(&status);
            if (status.maximum != test->expected_maximum ||
                status.current != test->expected_current) {
                passed = 0;
                break;
            }
        }
        say(passed ? "PASS: " : "FAIL: ");
        say(test->name);
        if (!passed) {
            ++failures;
            say(" expected current/max ");
            number(test->expected_current);
            say("/");
            number(test->expected_maximum);
            say(", got ");
            number(status.current);
            say("/");
            number(status.maximum);
        }
        say("\n");
    }
    ExitProcess(failures ? 1 : 0);
}
