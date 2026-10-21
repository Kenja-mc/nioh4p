/* SDK lifetime fixture. Released storage becomes unreadable deterministically. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

__declspec(dllexport) int32_t EOS_LobbyDetails_CopyInfo(void *handle, const void *options, void **out)
{
    (void)handle;
    (void)options;
    unsigned char *info = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!info) return 1;
    *(int32_t *)(info + 0) = 1;
    *(const char **)(info + 8) = "ownership-regression";
    *(uint32_t *)(info + 28) = 1;
    *(uint32_t *)(info + 32) = 3;
    *out = info;
    return 0;
}

__declspec(dllexport) void EOS_LobbyDetails_Info_Release(void *info)
{
    DWORD old;
    VirtualProtect(info, 4096, PAGE_NOACCESS, &old);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *reserved)
{
    (void)module; (void)reason; (void)reserved;
    return TRUE;
}
