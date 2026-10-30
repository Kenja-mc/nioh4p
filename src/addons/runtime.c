#include "runtime.h"

int n4p_config_int(const Nioh4pCoreApi *core, HMODULE module,
                   const char *key, const char *default_value,
                   int minimum, int maximum, int *value)
{
    char path[MAX_PATH], text[32];
    DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        core->log("addon config: cannot resolve module path");
        return 0;
    }
    char *extension = NULL;
    for (char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') extension = NULL;
        else if (*p == '.') extension = p;
    }
    if (!extension || (size_t)(extension - path) + 5 > sizeof path)
        return 0;
    memcpy(extension, ".ini", 5);
    length = GetPrivateProfileStringA("nioh4p", key, default_value,
                                      text, sizeof text, path);
    if (!length || length >= sizeof text - 1)
        goto invalid;
    unsigned result = 0;
    for (DWORD i = 0; i < length; i++) {
        unsigned digit = (unsigned char)text[i] - '0';
        if (digit > 9 || result > (2147483647u - digit) / 10)
            goto invalid;
        result = result * 10 + digit;
    }
    if (result < (unsigned)minimum || result > (unsigned)maximum)
        goto invalid;
    *value = (int)result;
    return 1;
invalid:
    core->log("addon config: invalid %s; expected decimal %d..%d; addon disabled",
              key, minimum, maximum);
    return 0;
}

uint8_t *n4p_manager(const Nioh4pCoreApi *core)
{
    uint8_t *const *slot = (uint8_t *const *)(core->game_base + N4P_MANAGER_RVA);
    if (!core->readable(slot, sizeof *slot) || !*slot ||
        !core->readable(*slot, 0x664))
        return NULL;
    return *slot;
}

unsigned n4p_party_count(const Nioh4pCoreApi *core)
{
    const uint8_t *root = (const uint8_t *)(core->game_base + N4P_ROOT_RVA);
    if (!core->readable(root, 3 * 0xa8 + 0xe9))
        return 0;
    unsigned count = 0;
    for (unsigned i = 0; i < N4P_PARTY_COUNT; i++)
        if (root[i * 0xa8 + 0xe8] == 2) count++;
    return count;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
