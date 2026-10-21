/* Consumer-side regression: the proxy must not release the game's CopyInfo result. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

__declspec(dllimport) HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, const void *, void **, void *);
__declspec(dllimport) int32_t EOS_LobbyDetails_CopyInfo(void *, const void *, void **);
__declspec(dllimport) void EOS_LobbyDetails_Info_Release(void *);

static void finish(const char *message, DWORD code)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, lstrlenA(message), &written, NULL);
    ExitProcess(code);
}

void mainCRTStartup(void)
{
    /* Force the proxy import; don't start its worker from a test-only entry point. */
    if (!GetModuleHandleA("dinput8.dll") && DirectInput8Create(0, 0, 0, 0, 0))
        finish("FAIL: proxy not loaded\n", 1);
    Sleep(1500);
    int32_t options = 1;
    void *info = 0;
    if (EOS_LobbyDetails_CopyInfo(0, &options, &info) != 0 || !info)
        finish("FAIL: CopyInfo did not return the owned result\n", 2);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(info, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_NOACCESS))
        finish("FAIL: CopyInfo result was released before its caller consumed it\n", 3);
    /* The SDK fixture writes MaxMembers at its documented x64 offset. */
    if (*(uint32_t *)((char *)info + 32) != 3)
        finish("FAIL: returned lobby capacity is corrupt\n", 4);
    EOS_LobbyDetails_Info_Release(info);
    if (!VirtualQuery(info, &mbi, sizeof mbi) || !(mbi.Protect & PAGE_NOACCESS))
        finish("FAIL: caller could not release its result\n", 5);
    finish("PASS: CopyInfo result survives until the caller releases it\n", 0);
}
