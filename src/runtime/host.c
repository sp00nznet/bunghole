/*
 * Bunghole in One - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks,
 * machine lock and guest-module binding; see its header). What is here is only
 * what is specific to this game: the two guest images, the install the engine
 * expects to find, the command line and the fault report. docs/host.md has the
 * reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000 (GOLF.EXE) and
 * 0x10000000 (00170001.DLL) are free when main() maps the images.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIRECTINPUT_VERSION 0x0300
#include <dinput.h>

#include "native32.h"
#include "recomp_trace.h"
#include "softdd.h"

extern const uint32_t bh_exe_entry_va, bh_dll_entry_va;     /* recomp_dispatch.c */

#define BH_EXE_BASE 0x00400000u
#define BH_DLL_BASE 0x10000000u
#define BH_DLL_NAME "00170001.DLL"

static DWORD g_watchdog_s;
static int   g_headless;
static uint32_t g_probe_va, g_probe_args[8];
static int   g_probe_n;
static char  g_game[MAX_PATH];           /* the CD's file tree, with a trailing '\' */
static char  g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 3];

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ------------------------------------------------------------- the guest */

/* The guest is GOLF.EXE in the game folder, not this host: its hInstance
 * (resources, window class) is GetModuleHandleA(NULL), and a path it builds
 * from its own file name has to land in the game folder. */
static void shim_GetModuleHandleA(void) {
    uint32_t name = ARG(0);
    uint32_t guest = name ? native32_module(gstr(name)) : BH_EXE_BASE;
    RET(guest ? guest : (uint32_t)(uintptr_t)GetModuleHandleA(gstr(name)), 1);
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == BH_EXE_BASE || h == BH_DLL_BASE) {
        char path[MAX_PATH];
        _snprintf(path, sizeof path - 1, "%s%s", g_game, h == BH_DLL_BASE ? BH_DLL_NAME : "GOLF.EXE");
        path[sizeof path - 1] = 0;
        uint32_t n = (uint32_t)strlen(path);
        if (size) {
            if (n >= size) n = size - 1;
            memcpy(out, path, n);
            out[n] = 0;
        }
        RET(n, 3);
    } else {
        RET(GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size), 3);
    }
}

static void shim_GetCommandLineA(void) { RET((uintptr_t)g_guest_cmdline, 0); }

/* The engine loads the game module by name ("Special Code Module Loader",
 * `%08lx.DLL` under the install path) and enters it through GetProcAddress.
 * The DLL is already mapped and bound as a guest; loading it means running its
 * lifted DllMain once, and its exports are guest VAs in the dispatch table. */
static int g_dll_attached;

static void shim_LoadLibraryA(void) {
    const char* name = gstr(ARG(0));
    uint32_t guest = native32_module(name);
    if (!guest) { RET((uintptr_t)LoadLibraryA(name), 1); return; }
    fprintf(stderr, "[module] LoadLibraryA(\"%s\") -> lifted 0x%08X\n", name, guest);
    if (guest == BH_DLL_BASE && !g_dll_attached) {
        uint32_t args[3] = { BH_DLL_BASE, DLL_PROCESS_ATTACH, 0 };
        g_dll_attached = 1;
        native32_call_guest(bh_dll_entry_va, 3, args);
        fprintf(stderr, "[module] DllMain(PROCESS_ATTACH) -> %u\n", g_eax);
    }
    RET(guest, 1);
}

static void shim_GetProcAddress(void) {
    uint32_t h = ARG(0);
    const char* name = (const char*)(uintptr_t)ARG(1);
    if (native32_in_guest(h)) {
        uint32_t va = native32_export(h, name);
        fprintf(stderr, "[module] GetProcAddress(0x%08X, \"%s\") -> 0x%08X\n", h,
                ARG(1) >> 16 ? name : "#", va);
        RET(va, 2);
    } else {
        RET((uintptr_t)GetProcAddress((HMODULE)(uintptr_t)h, name), 2);
    }
}

static void shim_FreeLibrary(void) {
    uint32_t h = ARG(0);
    RET(native32_in_guest(h) ? TRUE : FreeLibrary((HMODULE)(uintptr_t)h), 1);
}

/* DirectInput checks its HINSTANCE against the loader's module list, and the
 * guest image was mapped by hand, so 0x00400000 is E_INVALIDARG. The host's
 * own handle stands in: DirectInput only wants to know some module asked. */
typedef HRESULT (WINAPI *dicreate_t)(HINSTANCE, DWORD, void**, void*);
typedef HRESULT (WINAPI *di_createdev_t)(void* self, const GUID* g, void** dev, void* outer);
typedef HRESULT (WINAPI *did_setcoop_t)(void* self, HWND w, DWORD flags);
static di_createdev_t g_real_createdev;
static did_setcoop_t g_real_setcoop;

static void patch_vtbl(void* obj, int slot, void* fn, void** real) {
    void** vt = *(void***)obj;
    DWORD old;
    if (*real) return;                   /* one vtable per interface: patch once */
    *real = vt[slot];
    VirtualProtect(&vt[slot], 4, PAGE_READWRITE, &old);
    vt[slot] = fn;
    VirtualProtect(&vt[slot], 4, old, &old);
}

/* The game takes the mouse DISCL_EXCLUSIVE | DISCL_FOREGROUND (5). Exclusive
 * would hold the real mouse captive, and at the game's Acquire its window is
 * not in the foreground yet (hidden when headless, still a 1x1 popup when
 * not), so Acquire failed E_ACCESSDENIED and the game quit. It draws its own
 * cursor, so non-exclusive loses nothing.
 * ponytail: background, so the game also sees mouse motion while unfocused;
 * foreground plus a re-Acquire on WM_ACTIVATE if that bothers anyone. */
static HRESULT WINAPI hk_SetCooperativeLevel(void* self, HWND w, DWORD flags) {
    DWORD want = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    fprintf(stderr, "[input] SetCooperativeLevel 0x%lX -> 0x%lX\n", flags, want);
    return g_real_setcoop(self, w, want);
}

/* --click X,Y@F and --drag X,Y,DX,DY@F: scripted mouse input for headless
 * runs. The game reads the mouse as relative motion and keeps its own cursor,
 * so each gesture starts with a huge negative move (the cursor clamps at the
 * top-left) and a move to (X,Y). A click then holds the left button for four
 * frames; a drag presses, moves (DX,DY) over DRAG_FRAMES frames with the
 * button held (a putt: the ball goes the opposite way), and releases.
 * The timeline is in presented frames, not polls: the game reads the mouse
 * several times a frame, and a gesture timed in polls was over before it had
 * drawn once (the putt never started). */
#define MAX_CLICKS 32
#define DRAG_FRAMES 20
static struct { int x, y, dx, dy; long frame; long last; int moved; } g_clicks[MAX_CLICKS];
static int g_nclicks;

/* Did the putt do anything? The screen just after each drag against the
 * screen 150 frames later: a ball that moves takes the camera with it, and
 * one that does not leaves all but the cursor and a portrait unchanged (the
 * frozen ball, pcrecomp#20). tools/conformance.py reads the line. */
#define PUTT_WAIT 150
static uint16_t* g_putt_shot;
static long g_putt_at;

static void putt_watch(const uint8_t* bits, int w, int h, int pitch, long frame) {
    for (int i = 0; i < g_nclicks; i++)
        if ((g_clicks[i].dx || g_clicks[i].dy) && frame == g_clicks[i].frame + DRAG_FRAMES + 14) {
            if (!g_putt_shot) g_putt_shot = (uint16_t*)malloc((size_t)w * h * 2);
            for (int y = 0; y < h; y++) memcpy(g_putt_shot + y * w, bits + y * pitch, (size_t)w * 2);
            g_putt_at = frame;
        }
    if (g_putt_shot && frame == g_putt_at + PUTT_WAIT) {
        long changed = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                changed += ((const uint16_t*)(bits + y * pitch))[x] != g_putt_shot[y * w + x];
        fprintf(stderr, "[play] %ld%% of the screen changed in the %d frames after the putt\n",
                changed * 100 / ((long)w * h), PUTT_WAIT);
    }
}

typedef HRESULT (WINAPI *did_getstate_t)(void* self, DWORD cb, void* data);
typedef HRESULT (WINAPI *did_getdata_t)(void* self, DWORD cb, DIDEVICEOBJECTDATA* d, DWORD* n, DWORD f);
static did_getstate_t g_real_getstate;
static did_getdata_t g_real_getdata;

/* What the script adds to this poll: motion and the left button (-1 = leave). */
static int script_poll(LONG* dx, LONG* dy, int* button) {
    for (int i = 0; i < g_nclicks; i++) {
        long t = softdd_frames - g_clicks[i].frame;
        int drag = g_clicks[i].dx || g_clicks[i].dy, end = drag ? DRAG_FRAMES + 12 : 6;
        if (t < 0 || t > end || g_clicks[i].last == softdd_frames) continue;
        g_clicks[i].last = softdd_frames;                  /* one step per frame */
        *button = -1;
        if (t == 0) { *dx = -4000; *dy = -4000; }
        else if (t == 1) { *dx = g_clicks[i].x; *dy = g_clicks[i].y; }
        else if (!drag) { if (t == 2 || t == 6) *button = t == 2; }
        else if (t == 3 || t == end) *button = t == 3;
        else if (t >= 6 && g_clicks[i].moved < DRAG_FRAMES) {
            int k = ++g_clicks[i].moved;                   /* the k-th of DRAG_FRAMES slices */
            *dx = g_clicks[i].dx * k / DRAG_FRAMES - g_clicks[i].dx * (k - 1) / DRAG_FRAMES;
            *dy = g_clicks[i].dy * k / DRAG_FRAMES - g_clicks[i].dy * (k - 1) / DRAG_FRAMES;
        }
        if (t == 2) fprintf(stderr, "[input] %s at %d,%d (frame %ld)\n", drag ? "drag" : "click",
                            g_clicks[i].x, g_clicks[i].y, softdd_frames);
        return 1;
    }
    return 0;
}

static HRESULT WINAPI hk_GetDeviceState(void* self, DWORD cb, void* data) {
    static int once;
    if (!once++) fprintf(stderr, "[input] the game polls GetDeviceState (%lu bytes)\n", cb);
    HRESULT hr = g_real_getstate(self, cb, data);
    LONG dx = 0, dy = 0;
    int button;
    if (cb >= sizeof(DIMOUSESTATE) && script_poll(&dx, &dy, &button)) {
        DIMOUSESTATE* m = (DIMOUSESTATE*)data;
        m->lX += dx;
        m->lY += dy;
        if (button >= 0) m->rgbButtons[0] = button ? 0x80 : 0;
        hr = DI_OK;
    }
    return hr;
}

/* The game reads the mouse buffered, so a scripted step is appended as the
 * records a real mouse would have queued. `cb` is the caller's record size
 * (16 bytes for DirectX 3's DIDEVICEOBJECTDATA). */
static HRESULT WINAPI hk_GetDeviceData(void* self, DWORD cb, DIDEVICEOBJECTDATA* d, DWORD* n, DWORD f) {
    DWORD room = n ? *n : 0;
    HRESULT hr = g_real_getdata(self, cb, d, n, f);
    LONG dx = 0, dy = 0;
    int button;
    if (!d || !n || (f & DIGDD_PEEK) || !script_poll(&dx, &dy, &button)) return hr;
    struct { DWORD ofs, data; } add[3];
    int k = 0;
    if (dx) { add[k].ofs = DIMOFS_X; add[k++].data = (DWORD)dx; }
    if (dy) { add[k].ofs = DIMOFS_Y; add[k++].data = (DWORD)dy; }
    if (button >= 0) { add[k].ofs = DIMOFS_BUTTON0; add[k++].data = button ? 0x80 : 0; }
    for (int i = 0; i < k && *n < room; i++) {
        DIDEVICEOBJECTDATA* r = (DIDEVICEOBJECTDATA*)((uint8_t*)d + *n * cb);
        memset(r, 0, cb);
        r->dwOfs = add[i].ofs;
        r->dwData = add[i].data;
        r->dwTimeStamp = GetTickCount();
        (*n)++;
    }
    return hr == DI_BUFFEROVERFLOW ? hr : DI_OK;
}

static HRESULT WINAPI hk_CreateDevice(void* self, const GUID* g, void** dev, void* outer) {
    HRESULT hr = g_real_createdev(self, g, dev, outer);
    if (hr == 0 && *dev) {
        patch_vtbl(*dev, 13, (void*)hk_SetCooperativeLevel, (void**)&g_real_setcoop);
        patch_vtbl(*dev, 9, (void*)hk_GetDeviceState, (void**)&g_real_getstate);
        patch_vtbl(*dev, 10, (void*)hk_GetDeviceData, (void**)&g_real_getdata);
    }
    return hr;
}

static void shim_DirectInputCreateA(void) {
    static dicreate_t real;
    if (!real) real = (dicreate_t)GetProcAddress(LoadLibraryA("dinput.dll"), "DirectInputCreateA");
    HINSTANCE h = (HINSTANCE)(uintptr_t)ARG(0);
    void** out = (void**)(uintptr_t)ARG(2);
    if (native32_in_guest(ARG(0))) h = GetModuleHandleA(NULL);
    HRESULT hr = real(h, ARG(1), out, (void*)(uintptr_t)ARG(3));
    if (hr == 0 && *out) patch_vtbl(*out, 3, (void*)hk_CreateDevice, (void**)&g_real_createdev);
    RET(hr, 4);
}

/* ---------------------------------------------------------- the install */

/* The installer wrote HKLM\...\App Paths\Golf.exe with "Hard Drive Path" and
 * "CD-ROM Path". Both answer the game folder, so nothing is installed and
 * nothing touches the real registry (writing HKLM would need admin). */
#define FAKE_HKEY 0xB1D00001u

static void shim_RegOpenKeyExA(void) {
    const char* sub = gstr(ARG(1));
    size_t n = strlen(sub);
    if (n >= 8 && !_stricmp(sub + n - 8, "Golf.exe")) {
        MEM32(ARG(4)) = FAKE_HKEY;
        RET(ERROR_SUCCESS, 5);
        return;
    }
    RET(RegOpenKeyExA((HKEY)(uintptr_t)ARG(0), sub, ARG(2), ARG(3), (PHKEY)(uintptr_t)ARG(4)), 5);
}

static void shim_RegQueryValueExA(void) {
    if (ARG(0) != FAKE_HKEY) {
        RET(RegQueryValueExA((HKEY)(uintptr_t)ARG(0), gstr(ARG(1)), (LPDWORD)(uintptr_t)ARG(2),
                             (LPDWORD)(uintptr_t)ARG(3), (LPBYTE)(uintptr_t)ARG(4),
                             (LPDWORD)(uintptr_t)ARG(5)), 6);
        return;
    }
    const char* value = gstr(ARG(1));
    DWORD n = (DWORD)strlen(g_game) + 1, *size = (DWORD*)(uintptr_t)ARG(5);
    fprintf(stderr, "[install] %s = \"%s\"\n", value, g_game);
    if (ARG(3)) MEM32(ARG(3)) = REG_SZ;
    if (ARG(4) && size && *size < n) { *size = n; RET(ERROR_MORE_DATA, 6); return; }
    if (ARG(4)) memcpy((void*)(uintptr_t)ARG(4), g_game, n);
    if (size) *size = n;
    RET(ERROR_SUCCESS, 6);
}

static void shim_RegCloseKey(void) {
    RET(ARG(0) == FAKE_HKEY ? ERROR_SUCCESS : RegCloseKey((HKEY)(uintptr_t)ARG(0)), 1);
}

/* ------------------------------------------------------------ headless */

/* --headless: nothing reaches the screen (REPO_RULES section 13). */
static void shim_MessageBoxA(void) {
    fprintf(stderr, "[messagebox] %s: %s\n", gstr(ARG(2)), gstr(ARG(1)));
    RET(IDOK, 4);
}

static void shim_CreateWindowExA(void) {
    HWND h = CreateWindowExA(ARG(0), (LPCSTR)(uintptr_t)ARG(1), (LPCSTR)(uintptr_t)ARG(2),
                             ARG(3) & ~WS_VISIBLE, (int)ARG(4), (int)ARG(5), (int)ARG(6),
                             (int)ARG(7), (HWND)(uintptr_t)ARG(8), (HMENU)(uintptr_t)ARG(9),
                             (HINSTANCE)(uintptr_t)ARG(10), (LPVOID)(uintptr_t)ARG(11));
    fprintf(stderr, "[headless] CreateWindowExA(\"%s\", %dx%d) -> hidden hwnd %p\n",
            gstr(ARG(2)), (int)ARG(6), (int)ARG(7), (void*)h);
    RET((uintptr_t)h, 12);
}

static void shim_ShowWindow(void) { RET(0, 2); }

/* ------------------------------------------------------------ display */

/* Every mode: the game's DirectDraw is softdd, never the real one, so no
 * run ever changes the desktop's display mode. */
static void shim_DirectDrawCreate(void) {
    RET(softdd_create((void**)(uintptr_t)ARG(1)), 3);
}

static void shim_ExitProcess(void) {
    fprintf(stderr, "[exit] ExitProcess(%u) after %ld frames\n", ARG(0), softdd_frames);
    softdd_finish();
    fflush(stderr);
    ExitProcess(ARG(0));
}

#define GUEST_SHIMS \
    { "GetModuleHandleA", shim_GetModuleHandleA }, \
    { "GetModuleFileNameA", shim_GetModuleFileNameA }, \
    { "GetCommandLineA", shim_GetCommandLineA }, \
    { "LoadLibraryA", shim_LoadLibraryA }, \
    { "GetProcAddress", shim_GetProcAddress }, \
    { "FreeLibrary", shim_FreeLibrary }, \
    { "DirectInputCreateA", shim_DirectInputCreateA }, \
    { "DirectDrawCreate", shim_DirectDrawCreate }, \
    { "ExitProcess", shim_ExitProcess }, \
    { "RegOpenKeyExA", shim_RegOpenKeyExA }, \
    { "RegQueryValueExA", shim_RegQueryValueExA }, \
    { "RegCloseKey", shim_RegCloseKey }

/* native32 keeps one shim table: every bind gets the same array. */
static native32_shim_t g_shims[] = { GUEST_SHIMS };
static native32_shim_t g_headless_shims[] = {
    GUEST_SHIMS,
    { "MessageBoxA", shim_MessageBoxA },
    { "CreateWindowExA", shim_CreateWindowExA },
    { "ShowWindow", shim_ShowWindow },
};

/* ------------------------------------------------------------ reports */

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. */
static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n=== fault 0x%08lX at 0x%p ===\n", r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        fprintf(stderr, "  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                native32_in_guest(at) ? " (inside a guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    recomp_trace_flush();
    fprintf(stderr, "  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    recomp_dump_trace("fault");
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count);
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

static int map_and_bind(const char* file, uint32_t base, native32_shim_t* shims, int n) {
    char path[MAX_PATH];
    _snprintf(path, sizeof path - 1, "%s%s", g_game, file);
    path[sizeof path - 1] = 0;
    uint32_t span = native32_map(path, base);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", path, base); return 0; }
    printf("  mapped %s: 0x%08X-0x%08X\n", file, base, base + span);
    return native32_bind(base, shims, n) == 0;
}

int main(int argc, char** argv) {
    const char* game = "game\\disc";
    int run = 0;
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = softdd_headless = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            /* Absolute now: --run moves into the game folder before ffmpeg starts. */
            static char rec[MAX_PATH];
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            softdd_record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) softdd_stop_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--click") && i + 1 < argc && g_nclicks < MAX_CLICKS) {
            if (sscanf(argv[++i], "%d,%d@%ld", &g_clicks[g_nclicks].x, &g_clicks[g_nclicks].y,
                       &g_clicks[g_nclicks].frame) == 3) g_nclicks++;
        }
        else if (!strcmp(argv[i], "--drag") && i + 1 < argc && g_nclicks < MAX_CLICKS) {
            if (sscanf(argv[++i], "%d,%d,%d,%d@%ld", &g_clicks[g_nclicks].x, &g_clicks[g_nclicks].y,
                       &g_clicks[g_nclicks].dx, &g_clicks[g_nclicks].dy, &g_clicks[g_nclicks].frame) == 5)
                g_nclicks++;
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) softdd_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--call") && i + 1 < argc) {
            g_probe_va = strtoul(argv[++i], NULL, 0);
            while (i + 1 < argc && argv[i + 1][0] != '-' && g_probe_n < 8)
                g_probe_args[g_probe_n++] = strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: bunghole [--run] [--headless] [--record out.mp4] [--frames N] [--scale N]\n"
                   "                [--click X,Y@F] [--drag X,Y,DX,DY@F]\n"
                   "                [--game game\\disc] [--watchdog S] [--native-trace] [--callbacks]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH - 1, g_game, NULL);
    if (g_game[strlen(g_game) - 1] != '\\') strcat(g_game, "\\");
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%sGOLF.EXE", g_game);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    softdd_on_present = putt_watch;
    printf("Bunghole in One recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    native32_shim_t* shims = g_headless ? g_headless_shims : g_shims;
    int nshims = g_headless ? (int)(sizeof g_headless_shims / sizeof g_headless_shims[0])
                            : (int)(sizeof g_shims / sizeof g_shims[0]);
    /* The engine first: the DLL's imports bind to its exports. */
    if (!map_and_bind("GOLF.EXE", BH_EXE_BASE, shims, nshims) ||
        !map_and_bind(BH_DLL_NAME, BH_DLL_BASE, shims, nshims))
        return 1;

    if (g_probe_va) {
        /* --call: run one lifted function on the dwords given and show what it
         * returned, without booting the game (a CRT math routine, say). */
        int top = g_fp_top;
        native32_call_guest(g_probe_va, g_probe_n, g_probe_args);
        printf("  sub_%08X -> eax=%08X edx=%08X", g_probe_va, g_eax, g_edx);
        if (g_fp_top == top + 1) printf(" st(0)=%.17g", g_st[0]);
        printf("\n");
        return 0;
    }
    if (!run) {
        printf("\n(dry run: images mapped and bound; --run enters 0x%08X)\n", bh_exe_entry_va);
        return 0;
    }
    /* The engine opens its data relative to the install paths, but the CRT and
     * the file dialogs start from the working directory, as on the CD. */
    if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", bh_exe_entry_va);
    fflush(stdout);
    native32_call_guest(bh_exe_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
