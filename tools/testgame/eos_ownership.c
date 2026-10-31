/* Consumer-side regression: the proxy must not release the game's CopyInfo result. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

__declspec(dllimport) HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, const void *, void **, void *);
__declspec(dllimport) int32_t EOS_LobbyDetails_CopyInfo(void *, const void *, void **);
__declspec(dllimport) void EOS_LobbyDetails_Info_Release(void *);

/* IID_IDirectInput8W: the interface version the game asks for when it starts. */
static const unsigned char IID_DI8W[16] = {
    0x31, 0x80, 0x79, 0xBF, 0x3A, 0x48, 0xA2, 0x4D,
    0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 };

static void say(const char *message)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, lstrlenA(message), &written, NULL);
}

static void finish(const char *message, DWORD code)
{
    say(message);
    ExitProcess(code);
}

/* The address the loader resolved for our DirectInput8Create import: for a
 * dllimport function the compiler reads the import slot, so this is an address
 * inside the module that answered the import, not this exe's own code. Asking
 * GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS for the module containing an address
 * then names that module - not Wine's builtin dinput8.dll, and not the real one
 * the proxy forwards to, although all three are named dinput8.dll and are in the
 * same process. */
static HMODULE imported_dinput8(void)
{
    HMODULE m = 0;
    void *target = (void *)&DirectInput8Create;
    if (target)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char *)target, &m);
    return m;
}

/* Is m the nioh3p proxy? The exe imports dinput8.dll statically, so an import
 * slot always exists: with no override in place Wine's own dinput8.dll answers
 * it, the proxy's EOS hooks are then missing, and the checks below would still
 * pass. The old guard -
 * !GetModuleHandleA("dinput8.dll") && DirectInput8Create(0,0,0,0,0) - could
 * therefore never fire, so this fixture passed on a prefix that had no proxy in
 * it at all. Two facts are required, because either one alone can be satisfied
 * by something that is not the proxy:
 *
 *   - the exports are the proxy's: src/proxy/dinput8.def keeps the shipped
 *     Microsoft name table, DllInstall and GetdfDIJoystick included, while
 *     Wine's builtin dinput8.dll exports only the other five names (objdump -p
 *     on the Proton Experimental x86_64-windows dinput8.dll).
 *   - the file sits next to this executable, where install.sh and
 *     tools/smoketest.sh install the proxy; Wine maps its builtin from the
 *     prefix's system directory. This rejects another dinput8.dll with the same
 *     names, such as the shipped Microsoft one, elsewhere in the search order.
 *
 * DllInstall and GetdfDIJoystick are resolved, not called: the proxy forwards
 * them to the real dinput8, and that path is not what this fixture tests. */
static int proxy_loaded(HMODULE m)
{
    char exe[MAX_PATH], dll[MAX_PATH];
    DWORD elen, dlen, edir, ddir, i;
    if (!m)
        return 0;
    if (!GetProcAddress(m, "DllInstall") || !GetProcAddress(m, "GetdfDIJoystick"))
        return 0;
    elen = GetModuleFileNameA(NULL, exe, sizeof exe);
    dlen = GetModuleFileNameA(m, dll, sizeof dll);
    edir = elen;
    ddir = dlen;
    while (edir && exe[edir - 1] != '\\')
        edir--;
    while (ddir && dll[ddir - 1] != '\\')
        ddir--;
    if (!edir || edir != ddir)
        return 0;
    for (i = 0; i < edir; i++) {
        char a = exe[i], b = dll[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b)
            return 0;
    }
    return 1;
}

void mainCRTStartup(void)
{
    /* Make the static import real: the loader loads dinput8.dll - and with it
     * the proxy - before this code runs, and this call is the same one the game
     * makes first (the proxy forwards it to the real dinput8). */
    void *di = 0;
    char path[MAX_PATH];
    HMODULE m;
    DirectInput8Create(GetModuleHandleA(NULL), 0x0800, IID_DI8W, &di, 0);
    m = imported_dinput8();
    say("TEST: loaded dinput8 module = ");
    if (m && GetModuleFileNameA(m, path, sizeof path))
        say(path);
    else
        say("(NOT loaded)");
    say("\r\n");
    if (!proxy_loaded(m))
        finish("FAIL: proxy not loaded (the dinput8.dll that answered the import is neither "
               "the nioh3p proxy's export table nor the copy installed next to this exe)\n", 1);
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
