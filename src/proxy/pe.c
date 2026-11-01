#include "nioh3p_internal.h"

/* ------------------------------------------------------------ PE in-memory */

uint8_t *g_base;
size_t g_img_size;

IMAGE_NT_HEADERS64 *nt(void)
{
    if (!g_base)
        return NULL;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    if (dos->e_lfanew <= 0 || (uint32_t)dos->e_lfanew > 0x1000)
        return NULL;
    IMAGE_NT_HEADERS64 *n = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    if (n->Signature != IMAGE_NT_SIGNATURE || n->OptionalHeader.Magic != 0x20b)
        return NULL;
    return n;
}

/* Nioh 1.24.08: internal offsets are not a Steam SDK ABI. Every game-side
 * patch and hook refuses a different executable rather than writing through
 * an unverified object layout. */
int exe_supported(void)
{
    IMAGE_NT_HEADERS64 *image = nt();
    return image && image->FileHeader.TimeDateStamp == 0x6307a81b &&
        image->OptionalHeader.SizeOfImage == 0x306e000 &&
        image->OptionalHeader.AddressOfEntryPoint == 0x303b310 &&
        image->OptionalHeader.CheckSum == 0x19c00f0;
}

/* log2 without libm (entropy diagnostics only, research build) */
#ifdef NIOH3P_RESEARCH
static double log2a(double x)
{
    if (x <= 0.0)
        return 0.0;
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 1.0)  { x *= 2.0; e--; }
    double y = (x - 1.0) / (x + 1.0), y2 = y * y, t = y, s = 0.0;
    for (int i = 1; i <= 13; i += 2) {
        s += t / (double)i;
        t *= y2;
    }
    return (double)e + 2.0 * s / 0.6931471805599453;
}
#endif

#ifdef NIOH3P_RESEARCH
/* The image dumper is research-only: it reads the 27 MB on-disk exe, checks
 * its entropy and walks the module image. The player build has no use for it. */
static IMAGE_SECTION_HEADER *find_section(const char *name)
{
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h)
        return NULL;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(h);
    for (int i = 0; i < h->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((const char *)s->Name, name, 8))
            return s;
    return NULL;
}

static double entropy(const uint8_t *p, size_t n)
{
    size_t hist[256];
    memset(hist, 0, sizeof hist);
    for (size_t i = 0; i < n; i++)
        hist[p[i]]++;
    double e = 0.0;
    for (int i = 0; i < 256; i++) {
        if (!hist[i])
            continue;
        double pr = (double)hist[i] / (double)n;
        e -= pr * log2a(pr);
    }
    return e;
}

static uint8_t *read_whole_file(const char *path, size_t *size_out)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0) {
        CloseHandle(h);
        return NULL;
    }
    uint8_t *b = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    if (!b || !ReadFile(h, b, sz, &got, NULL) || got != sz) {
        CloseHandle(h);
        if (b) VirtualFree(b, 0, MEM_RELEASE);
        return NULL;
    }
    CloseHandle(h);
    *size_out = sz;
    return b;
}

static int write_file(const char *path, const void *data, size_t n)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &wrote, NULL);
    CloseHandle(h);
    return ok && wrote == n;
}

/* -------------------------------------------------- decryption wait + dump */

/* 1 once the runtime .text no longer matches the on-disk (encrypted) copy. */
static int text_is_decrypted(uint8_t *disk, size_t disk_size)
{
    IMAGE_SECTION_HEADER *s = find_section(".text");
    if (!s)
        return 0;
    size_t n = s->Misc.VirtualSize < 0x10000 ? s->Misc.VirtualSize : 0x10000;
    uint8_t *mem = g_base + s->VirtualAddress;
    if (s->PointerToRawData + n > disk_size)
        return 0;
    size_t diff = 0;
    for (size_t i = 0; i < n; i++)
        if (mem[i] != disk[s->PointerToRawData + i])
            diff++;
    double frac = (double)diff / (double)n;
    double hm = entropy(mem, n);
    LOG("decrypt-probe: .text delta=%.1f%% runtime-entropy=%.3f", frac * 100.0, hm);
    if (hm >= 7.5)
        return 0;                       /* still (or again) encrypted */
    if (frac < 0.02)
        return 1;                       /* never encrypted: plaintext at rest */
    return frac > 0.30;                 /* unpacked at runtime */
}

static void dump_image(const char *tag)
{
    IMAGE_NT_HEADERS64 *h = nt();
    if (!h)
        return;
    g_img_size = h->OptionalHeader.SizeOfImage;
    uint8_t *buf = VirtualAlloc(NULL, g_img_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        LOG("dump: VirtualAlloc(%zu) failed", g_img_size);
        return;
    }
    memset(buf, 0, g_img_size);
    size_t copied = 0, unreadable = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (size_t off = 0; off < g_img_size; ) {
        if (!VirtualQuery(g_base + off, &mbi, sizeof mbi))
            break;
        size_t chunk = (size_t)mbi.RegionSize;
        if (off + chunk > g_img_size)
            chunk = g_img_size - off;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) {
            memcpy(buf + off, g_base + off, chunk);
            copied += chunk;
        } else {
            unreadable += chunk;
        }
        off += chunk;
    }
    char path[MAX_PATH];
    fmt(path, MAX_PATH, "%s\\nioh3p_image_%s.bin", g_out_dir, tag);
    if (write_file(path, buf, g_img_size))
        LOG("dump: wrote %s (%u bytes, base %p, readable %u, unreadable %u)",
            path, (unsigned)g_img_size, g_base, (unsigned)copied, (unsigned)unreadable);
    else
        LOG("dump: FAILED to write %s", path);

    /* sidecar manifest so the raw image can be loaded at the right VA */
    char meta[1024];
    int n = fmt(meta, sizeof meta,
        "{\"module\":\"nioh.exe\",\"image_base\":\"0x%p\",\"image_size\":%u,"
        "\"entry_rva\":\"0x%x\",\"sections\":[",
        g_base, (unsigned)g_img_size, h->OptionalHeader.AddressOfEntryPoint);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(h);
    for (int i = 0; i < h->FileHeader.NumberOfSections && n < (int)sizeof(meta) - 128; i++, s++)
        n += fmt(meta + n, (int)sizeof(meta) - n,
            "%s{\"name\":\"%.8s\",\"va\":\"0x%x\",\"vsize\":\"0x%x\"}",
            i ? "," : "", (const char *)s->Name,
            (unsigned)s->VirtualAddress, (unsigned)s->Misc.VirtualSize);
    n += fmt(meta + n, (int)sizeof(meta) - n, "]}");
    char mpath[MAX_PATH];
    fmt(mpath, MAX_PATH, "%s\\nioh3p_image_%s.json", g_out_dir, tag);
    write_file(mpath, meta, (size_t)n);
    VirtualFree(buf, 0, MEM_RELEASE);
}
#endif

/* Does the loaded proxy DLL carry an RT_MANIFEST resource? Walk the resource
 * directory root of this module's PE image; the startup log then proves that
 * the embedded manifest actually reached the linked DLL. */
int manifest_resource_present(HMODULE self)
{
    uint8_t *base = (uint8_t *)self;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS64 *n = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (n->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    DWORD rva = n->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
    if (!rva)
        return 0;
    IMAGE_RESOURCE_DIRECTORY *root = (IMAGE_RESOURCE_DIRECTORY *)(base + rva);
    IMAGE_RESOURCE_DIRECTORY_ENTRY *entry = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(root + 1);
    unsigned count = root->NumberOfNamedEntries + root->NumberOfIdEntries;
    for (unsigned i = 0; i < count; i++) {
        DWORD name = entry[i].Name;   /* Id in the low 16 bits */
        if (!(name & IMAGE_RESOURCE_NAME_IS_STRING) && (name & 0xFFFF) == 24)
            return 1;                 /* RT_MANIFEST */
    }
    return 0;
}

#ifdef NIOH3P_RESEARCH
static uint8_t *g_dump_disk;
static size_t g_dump_disk_size;

DWORD WINAPI dump_worker(LPVOID unused)
{
    (void)unused;
    int waited = 0;
    while (g_dump_disk && waited < 60000) {
        if (text_is_decrypted(g_dump_disk, g_dump_disk_size))
            break;
        /* a copy started outside Steam dies on the DRM stub within ~30 ms,
         * so poll tightly for the first two seconds */
        int step = waited < 2000 ? 10 : 200;
        Sleep((DWORD)step);
        waited += step;
    }
    if (g_dump_disk) {
        VirtualFree(g_dump_disk, 0, MEM_RELEASE);
        g_dump_disk = 0;
    }
    LOG("worker: dump trigger after %d ms", waited);
    dump_image("boot");
    if (g_cfg.redump_after_s > 0) {
        Sleep((DWORD)g_cfg.redump_after_s * 1000);
        dump_image("settled");
    }
    /* VMProtect decrypts page by page as the game runs, so a single dump
     * only holds what had executed by then: keep taking snapshots. */
    if (g_cfg.dump_every_s > 0) {
        for (int n = 1; n <= 12; n++) {
            char tag[32];
            fmt(tag, sizeof tag, "t%ds", n * g_cfg.dump_every_s);
            Sleep((DWORD)g_cfg.dump_every_s * 1000);
            dump_image(tag);
        }
    }
    return 0;
}

uint8_t *pe_disk_probe(size_t *size_out)
{
    char exe[MAX_PATH];
    size_t disk_size = 0;
    uint8_t *disk = 0;
    if (g_cfg.dump_image) {
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        disk = read_whole_file(exe, &disk_size);
        LOG("worker: on-disk exe %s (%u bytes) %s", exe, (unsigned)disk_size,
            disk ? "read" : "UNREADABLE");
        if (disk) {
            IMAGE_SECTION_HEADER *s = find_section(".text");
            if (s && s->PointerToRawData + 0x10000 <= disk_size)
                LOG("worker: on-disk .text entropy=%.3f (8.0 == encrypted)",
                    entropy(disk + s->PointerToRawData, 0x10000));
        }
    }
    *size_out = disk_size;
    return disk;
}

void pe_dump_handoff(uint8_t *disk, size_t size)
{
    g_dump_disk = disk;
    g_dump_disk_size = size;
}
#endif
