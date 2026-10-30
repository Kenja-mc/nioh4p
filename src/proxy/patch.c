/* nioh3p core: patch (split from nioh3p_proxy.c) */
#include "nioh3p_internal.h"

/* ---------------------------------------------------------------- hooking */

Hook g_hooks[MAX_HOOKS];
int g_hook_count;

int add_slot(Hook *h, void **slot)
{
    if (h->nslots >= MAX_SLOTS) {
        LOG("hook: slot table full for %s", h->name);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        LOG("hook: VirtualProtect(%p) failed for %s (err %lu)", slot, h->name, GetLastError());
        return 0;
    }
    /* The detour reads h->orig, and the game may enter it as soon as a slot
     * holds it: publish orig before the slot points at the detour. x86 keeps
     * the store order; the barrier keeps the compiler from reordering. */
    if (!h->orig)
        h->orig = *slot;
    __asm__ volatile("" ::: "memory");
    *slot = h->detour;
    VirtualProtect(slot, sizeof(void *), old, &old);
    h->slot[h->nslots++] = slot;
    return 1;
}

int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.Protect & PAGE_GUARD)
        return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
        return 0;
    return (size_t)((const uint8_t *)mbi.BaseAddress + mbi.RegionSize - (const uint8_t *)p) >= n;
}

/* A steam vtable slot must point at committed, non-guard executable memory
 * before the proxy overwrites it. */
int executable_ptr(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.Protect & PAGE_GUARD)
        return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* The import table as the loader built it: authoritative, no scan needed. */
/* A few imports are not matched by name (the descriptor spells the symbol
 * differently, or the slot already holds a resolved address from elsewhere).
 * The slot is still the right place to intercept, so find it by value: this is
 * also the only fallback that can see a slot at all, since IAT pages are
 * read-only and the writable-pointer scan can never reach them. */
static void **find_iat_slot_by_value(void *expect)
{
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h)
        return NULL;
    DWORD imp_rva = h->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva)
        return NULL;
    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + imp_rva);
    for (; desc->Name; desc++) {
        const char *dll = (const char *)(g_base + desc->Name);
        IMAGE_THUNK_DATA64 *oft = (IMAGE_THUNK_DATA64 *)(g_base + (desc->OriginalFirstThunk
                                    ? desc->OriginalFirstThunk : desc->FirstThunk));
        IMAGE_THUNK_DATA64 *ft = (IMAGE_THUNK_DATA64 *)(g_base + desc->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if ((void *)ft->u1.Function != expect)
                continue;
            LOG("hook: slot %p in %s already holds %p", (void **)&ft->u1.Function, dll, expect);
            return (void **)&ft->u1.Function;
        }
    }
    return NULL;
}

static void **find_iat_slot(const char *dll_name, const char *symbol, void *expect, int *saw_iat)
{
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h)
        return NULL;
    DWORD imp_rva = h->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva)
        return NULL;
    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + imp_rva);
    for (; desc->Name; desc++) {
        const char *dll = (const char *)(g_base + desc->Name);
        if (stricmp_a(dll, dll_name))
            continue;
        *saw_iat = 1;
        IMAGE_THUNK_DATA64 *oft = (IMAGE_THUNK_DATA64 *)(g_base + (desc->OriginalFirstThunk
                                    ? desc->OriginalFirstThunk : desc->FirstThunk));
        IMAGE_THUNK_DATA64 *ft = (IMAGE_THUNK_DATA64 *)(g_base + desc->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                continue;
            const char *nm = (const char *)(g_base + oft->u1.AddressOfData + 2);
            if (strcmp(nm, symbol))
                continue;
            LOG("hook: import table slot for %s = %p (module exports %p)",
                symbol, (void *)ft->u1.Function, expect);
            if ((void *)ft->u1.Function == expect)
                return (void **)&ft->u1.Function;
            LOG("hook: slot holds a different address, the runtime import table was "
                "rebuild; falling back to the pointer scan");
            return NULL;
        }
    }
    return NULL;
}

/* Fallback: any writable location in the main module storing the EOS entry
 * point is a call path we can redirect. */
static int scan_and_patch_range(Hook *h, uint8_t *mod_base, size_t mod_size)
{
    int hits = 0;
    for (size_t off = 0; off < mod_size; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(mod_base + off, &mbi, sizeof mbi))
            break;
        size_t chunk = (size_t)mbi.RegionSize;
        if (off + chunk > mod_size)
            chunk = mod_size - off;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            uint8_t *b = mod_base + off;
            for (size_t i = 0; i + sizeof(void *) <= chunk; i += sizeof(void *)) {
                void **p = (void **)(b + i);
                if (*p == h->target && add_slot(h, p)) {
                    LOG("hook: pointer scan patched %p (%s)", p, h->name);
                    hits++;
                }
            }
        }
        off += chunk;
    }
    return hits;
}

/* Re-apply after the game or the protector restores a slot. */
int repair_slots(Hook *h)
{
    int fixed = 0;
    for (int i = 0; i < h->nslots; i++) {
        if (*h->slot[i] == h->target) {
            DWORD old;
            if (VirtualProtect(h->slot[i], sizeof(void *), PAGE_READWRITE, &old)) {
                *h->slot[i] = h->detour;
                VirtualProtect(h->slot[i], sizeof(void *), old, &old);
                fixed++;
            }
        }
    }
    return fixed;
}

int g_hook_fail;

int install_hook(const char *dll_name, const char *name, void *target,
                        void *detour, void **orig_out)
{
    g_hook_fail = HOOK_FAIL_NONE;
    for (int i = 0; i < g_hook_count; i++)      /* installer runs repeatedly */
        if (!strcmp(g_hooks[i].name, name))
            return 1;
    if (g_hook_count >= MAX_HOOKS) {
        g_hook_fail = HOOK_FAIL_TABLE_FULL;
        LOG("hook: table full, cannot hook %s", name);
        return 0;
    }
    Hook *h = &g_hooks[g_hook_count];
    h->name = name;
    h->target = target;
    h->detour = detour;
    h->orig = target;
    h->nslots = 0;
    *orig_out = target;

    int saw_iat = 0;
    void **slot = find_iat_slot(dll_name, name, target, &saw_iat);
    if (!slot)
        slot = find_iat_slot_by_value(target);      /* by-value fallback */
    if (slot && add_slot(h, slot))
        LOG("hook: %s hooked through the import table (%p -> %p)", name, slot, detour);
    else
        LOG("hook: %s not usable in the import table (present=%d), scanning writable memory",
            name, saw_iat);
    if (h->nslots == 0) {
        int hits = scan_and_patch_range(h, g_base, g_img_size);
        LOG("hook: pointer scan patched %d location(s) for %s", hits, name);
    }
    if (h->nslots == 0) {
        g_hook_fail = HOOK_FAIL_NO_CALLSITE;
        LOG("hook: %s NOT hooked (no call site found in this build)", name);
        return 0;
    }
    g_hook_count++;
    return 1;
}

/* Publish a complete branch without exposing a partially written instruction.
 * The caller prepares its trampoline and any target state before this call. */
int publish_code16(uint8_t *site, const uint8_t *expected,
                          const uint8_t *patch, const char *name)
{
    typedef struct { uint64_t lo, hi; } Pair16;
    if ((uintptr_t)site & 15) {
        LOG("game patch: %s is not 16-byte aligned; not installed", name);
        return 0;
    }
    Pair16 want, have;
    memcpy(&want, expected, sizeof want);
    memcpy(&have, patch, sizeof have);
    DWORD old;
    if (!VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &old)) {
        LOG("game patch: cannot write %s (err %lu)", name, GetLastError());
        return 0;
    }
    unsigned char ok;
    __asm__ volatile(
        "lock cmpxchg16b %1\n\t"
        "sete %0"
        : "=q"(ok), "+m"(*(Pair16 *)site), "+a"(want.lo), "+d"(want.hi)
        : "b"(have.lo), "c"(have.hi)
        : "cc", "memory");
    if (ok)
        FlushInstructionCache(GetCurrentProcess(), site, 16);
    if (!VirtualProtect(site, 16, PAGE_EXECUTE_READ, &old))
        LOG("game patch: cannot restore execute protection for %s (err %lu)",
            name, GetLastError());
    if (!ok)
        LOG("game patch: %s changed during install; not installed", name);
    return ok;
}

void abs_jmp(uint8_t *at, const void *to)
{
    uintptr_t target = (uintptr_t)to;
    at[0] = 0xff; at[1] = 0x25; at[2] = at[3] = at[4] = at[5] = 0;
    memcpy(at + 6, &target, 8);
}

/* 1 installed, 0 bytes not (yet) the expected plaintext, -1 failed. */
int inline_install(const InlineHook *h)
{
    uint8_t *site = g_base + h->rva;
    if (h->n < 14 || h->n > sizeof h->bytes || ((uintptr_t)site & 15)) {
        LOG("game tokoyo: unsupported branch span for %s", h->name);
        return -1;
    }
    SIZE_T span = h->n < 16 ? 16 : h->n;
    if (!readable(site, span) || memcmp(site, h->bytes, h->n))
        return 0;
    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    if (!tramp) {
        LOG("game tokoyo: no trampoline for %s (err %lu)", h->name, GetLastError());
        return -1;
    }
    unsigned body = h->relocate ? h->relocate(tramp) : h->n;
    if (!h->relocate)
        memcpy(tramp, h->bytes, h->n);
    abs_jmp(tramp + body, site + h->n);
    if (!VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old)) {
        LOG("game tokoyo: cannot seal the %s trampoline (err %lu)", h->name, GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    uint8_t expected[16], patch[16];
    memcpy(expected, site, sizeof expected);
    /* The memcmp above proved bytes 0..n-1 equal the site, so the site bytes
     * are exactly the comparison word cmpxchg16b needs. */
    memcpy(patch, expected, sizeof patch);
    abs_jmp(patch, h->detour);
    /* Bytes 14 onward stay native. The detour jumps past the complete
     * replayed prologue, so padding them with separate stores is unnecessary. */
    *h->orig = tramp;
    if (!publish_code16(site, expected, patch, h->name)) {
        *h->orig = NULL;
        VirtualFree(tramp, 0, MEM_RELEASE);
        return -1;
    }
    return 1;
}

uint8_t *alloc_rel32(const uint8_t *site, SIZE_T size)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const uintptr_t from = (uintptr_t)site + 5;
    uintptr_t low = from > INT32_MAX ? from - INT32_MAX : 0;
    uintptr_t high = from + INT32_MAX;
    if (low < (uintptr_t)system.lpMinimumApplicationAddress)
        low = (uintptr_t)system.lpMinimumApplicationAddress;
    if (high > (uintptr_t)system.lpMaximumApplicationAddress)
        high = (uintptr_t)system.lpMaximumApplicationAddress;
    const uintptr_t granularity = system.dwAllocationGranularity;
    for (uintptr_t cursor = low; cursor < high; ) {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery((const void *)cursor, &region, sizeof region))
            break;
        uintptr_t end = (uintptr_t)region.BaseAddress + region.RegionSize;
        if (end <= cursor)
            break;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = (cursor + granularity - 1) & ~(granularity - 1);
            if (candidate < end && candidate < high &&
                size <= end - candidate && size <= high - candidate) {
                uint8_t *mem = VirtualAlloc((void *)candidate, size,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (mem)
                    return mem;
            }
        }
        cursor = end;
    }
    return NULL;
}
