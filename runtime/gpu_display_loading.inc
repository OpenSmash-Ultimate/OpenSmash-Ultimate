/* The loading animation in the black window (ported from ssbu-recomp's runtime/loading_anim.c, its U3), Windows. Included by gpu_display.c; the PNGs are looked for beside the exe, then in media\ (the repository's).
 *
 * From the moment the window exists until the game's first present, nothing draws into it: the boot (main.elf, the game
 * DLL, the Vulkan device and its pipeline cache, the container store, the game's own boot) left it black for seconds, and
 * on a first start for minutes. The window thread paints it meanwhile with GDI+: the owner's Donkey Kong silhouette
 * (media/loading_kong.png) with his arm (media/loading_arm.png) turning counter-clockwise around the shoulder, winding up
 * the Giant Punch, small in the bottom-right corner on black, and beside it the step the title bar shows
 * (display_title_note) when one is known. Both PNGs were rendered from the owner's SVGs at 4 px per SVG unit, the whole
 * viewBox each, so the constants below are in SVG units.
 *
 * It stops for good at the first present (display_present_plan / display_before_present / display_pace, game's thread),
 * under a lock the painter holds while it paints: once the first present has begun, no GDI pixel is drawn into the window
 * again, so the animation never covers a Vulkan (or OpenGL) frame. gdiplus.dll is loaded at run time (no link change);
 * no PNG, no gdiplus.dll, a test window off-screen or SSBU_LOADING_ANIM=0: the window stays black as before. Host only:
 * nothing the game reads changes. */

/* ---- the art, as the owner approved it (2026-10-21; his reference: Pictures\OpenSmash\loading\kong_arm_v4_fast.gif):
 * the arm's point LA_ARM_PIN (arm SVG units; bottom-left of the arm, outside its shape) is put on Kong's shoulder
 * LA_KONG_PIN (kong SVG units) and the arm turns around it, counter-clockwise, one turn in LA_TURN_MS. Drawn in this
 * order: the arm (behind), Kong's solid black silhouette (media/loading_kong_back.png: Kong's outline closed and filled,
 * so the arm never shows through his eyes, mouth or inner lines), Kong. (At 8 px per unit: pivot (400, 300) px on Kong,
 * (0, 320) px on the arm.) */
#define LA_ARM_PIN_X 0.0f
#define LA_ARM_PIN_Y 40.0f
#define LA_KONG_PIN_X 50.0f
#define LA_KONG_PIN_Y 37.5f
#define LA_TURN_MS 600                     /* one turn of the arm: 600 degrees a second */
#define LA_ARM_REACH_UP 13.0f              /* the arm's turn goes this far above Kong's viewBox (its farthest point is 50 units from the pivot) */
#define LA_KONG_W 104.34f                  /* the viewBoxes */
#define LA_KONG_H 105.13f
#define LA_ARM_W 49.28f
#define LA_ARM_H 41.89f
#define LA_SIZE 0.15f                      /* the kong viewBox's height, of the client height */
#define LA_MARGIN 0.025f                   /* from the bottom-right corner, of the client height */

typedef struct { UINT32 version; void *debug_cb; BOOL no_bg_thread, no_codecs; } LaGdipInput;
typedef int (WINAPI *la_startup_t)(ULONG_PTR *, const LaGdipInput *, void *);
typedef int (WINAPI *la_load_t)(const WCHAR *, void **);
typedef int (WINAPI *la_from_hdc_t)(HDC, void **);
typedef int (WINAPI *la_set_t)(void *, int);
typedef int (WINAPI *la_xy_t)(void *, float, float, int);
typedef int (WINAPI *la_rot_t)(void *, float, int);
typedef int (WINAPI *la_draw_t)(void *, void *, float, float, float, float);
typedef int (WINAPI *la_del_t)(void *);
typedef int (WINAPI *la_reset_t)(void *);
static struct {
    la_from_hdc_t from_hdc; la_set_t smoothing, interp, pixoff; la_xy_t translate, scale; la_rot_t rotate; la_draw_t draw; la_del_t del; la_reset_t reset;
    void *kong, *arm, *back;
    int on;                                /* window thread: painting (timer running) */
    RECT last;                             /* the client part painted last (bottom-right; the timer repaints it) */
    HFONT font; int font_h;
    LARGE_INTEGER t0;
} LA;
static SRWLOCK g_la_lock = SRWLOCK_INIT;
static volatile LONG g_la_off;             /* 1: the first present has begun (or no animation): never paint again */
static volatile LONG g_la_first;           /* 1: the first present has begun (la_stop ran) */
static SRWLOCK g_la_note_lock = SRWLOCK_INIT;
static char g_la_note[200];                /* display_title_note's text, up to its " (" */
static LONG64 g_la_t_window;               /* QPC when the window was made, for the black-time line */
#define LA_TIMER 0x5a1

static double la_since_process_start(void) {      /* seconds since the process was created */
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b; a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime; b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (double)(b.QuadPart - a.QuadPart) / 1e7 : 0;
}

/* display_title_note: the step, for the line beside the animation */
static void la_note(const char *note) {
    AcquireSRWLockExclusive(&g_la_note_lock);
    g_la_note[0] = 0;
    if (note) {
        size_t n = strcspn(note, "(");
        while (n && note[n - 1] == ' ') n--;
        if (n >= sizeof g_la_note) n = sizeof g_la_note - 1;
        memcpy(g_la_note, note, n); g_la_note[n] = 0;
        if (g_la_note[0] >= 'a' && g_la_note[0] <= 'z') g_la_note[0] -= 32;
    }
    ReleaseSRWLockExclusive(&g_la_note_lock);
}

/* window thread, after the window is made */
static void la_start(HWND h, int visible) {
    QueryPerformanceCounter(&LA.t0);
    g_la_t_window = LA.t0.QuadPart;
    printf("display: window made %.2f s after the process started\n", la_since_process_start());
    const char *e = getenv("SSBU_LOADING_ANIM");
    if (!visible || (e && atoi(e) == 0) || getenv("SSBU_VK_SELFTEST")) { InterlockedExchange(&g_la_off, 1); return; }
    HMODULE g = LoadLibraryA("gdiplus.dll");
    la_startup_t startup = g ? (la_startup_t)(void *)GetProcAddress(g, "GdiplusStartup") : NULL;
    la_load_t load = g ? (la_load_t)(void *)GetProcAddress(g, "GdipLoadImageFromFile") : NULL;
    LA.from_hdc = g ? (la_from_hdc_t)(void *)GetProcAddress(g, "GdipCreateFromHDC") : NULL;
    LA.smoothing = g ? (la_set_t)(void *)GetProcAddress(g, "GdipSetSmoothingMode") : NULL;
    LA.interp = g ? (la_set_t)(void *)GetProcAddress(g, "GdipSetInterpolationMode") : NULL;
    LA.pixoff = g ? (la_set_t)(void *)GetProcAddress(g, "GdipSetPixelOffsetMode") : NULL;
    LA.translate = g ? (la_xy_t)(void *)GetProcAddress(g, "GdipTranslateWorldTransform") : NULL;
    LA.scale = g ? (la_xy_t)(void *)GetProcAddress(g, "GdipScaleWorldTransform") : NULL;
    LA.rotate = g ? (la_rot_t)(void *)GetProcAddress(g, "GdipRotateWorldTransform") : NULL;
    LA.draw = g ? (la_draw_t)(void *)GetProcAddress(g, "GdipDrawImageRect") : NULL;
    LA.del = g ? (la_del_t)(void *)GetProcAddress(g, "GdipDeleteGraphics") : NULL;
    LA.reset = g ? (la_reset_t)(void *)GetProcAddress(g, "GdipResetWorldTransform") : NULL;
    if (!startup || !load || !LA.from_hdc || !LA.smoothing || !LA.interp || !LA.pixoff || !LA.translate || !LA.scale || !LA.rotate || !LA.draw || !LA.del || !LA.reset) {
        printf("display: loading animation off: gdiplus.dll not usable\n"); InterlockedExchange(&g_la_off, 1); return;
    }
    ULONG_PTR token; LaGdipInput in = {1, NULL, FALSE, FALSE};
    WCHAR p[MAX_PATH + 32]; DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH);
    while (n && p[n - 1] != L'\\' && p[n - 1] != L'/') n--;
    if (startup(&token, &in, NULL) == 0 && n) {
        for (int d = 0; d < 2 && !(LA.kong && LA.arm && LA.back); d++) {   /* beside the exe, else media\ (the working folder) */
            WCHAR q[MAX_PATH + 64];
            size_t m;
            if (d == 0) { wmemcpy(q, p, n); q[n] = 0; } else wcscpy(q, L"media\\");
            m = wcslen(q);
            wcscpy(q + m, L"loading_kong.png"); if (load(q, &LA.kong)) LA.kong = NULL;
            wcscpy(q + m, L"loading_arm.png"); if (load(q, &LA.arm)) LA.arm = NULL;
            wcscpy(q + m, L"loading_kong_back.png"); if (load(q, &LA.back)) LA.back = NULL;
        }
    }
    if (!LA.kong || !LA.arm || !LA.back) { printf("display: loading animation off: loading_kong.png / loading_arm.png / loading_kong_back.png not beside the exe\n"); InterlockedExchange(&g_la_off, 1); return; }
    if (g_la_off) return;                  /* (a present already) */
    LA.on = 1;
    SetTimer(h, LA_TIMER, 33, NULL);       /* about 30 pictures a second */
    InvalidateRect(h, NULL, TRUE);
}

/* window thread: one picture into the bottom-right corner (WM_PAINT) */
static void la_paint(HWND h, HDC dc) {
    RECT cr; GetClientRect(h, &cr);
    int cw = cr.right, ch = cr.bottom;
    if (cw < 64 || ch < 64) return;
    float s = (float)ch * LA_SIZE / LA_KONG_H;               /* pixels per SVG unit */
    int margin = (int)((float)ch * LA_MARGIN), ih = (int)(LA_KONG_H * s) + 1, iw = (int)(LA_KONG_W * s) + 1;
    int fh = ch / 36 < 13 ? 13 : ch / 36;
    int reach = (int)(LA_ARM_REACH_UP * s) + 1;
    RECT r = {cw * 2 / 5, ch - margin - ih - reach, cw, ch};  /* the part repainted: the text's width and the figure, with the arm's turn above it */
    if (r.top < 0) r.top = 0;
    int w = r.right - r.left, hh = r.bottom - r.top;
    if (w <= 0 || hh <= 0) return;
    LA.last = r;
    if (fh != LA.font_h) {
        if (LA.font) DeleteObject(LA.font);
        LA.font = CreateFontA(-fh, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
        LA.font_h = fh;
    }
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, w, hh), old = (HBITMAP)SelectObject(m, bm);
    RECT all = {0, 0, w, hh};
    FillRect(m, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    double ms = (double)(now.QuadPart - LA.t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    float turn = (float)fmod(ms, (double)LA_TURN_MS) / (float)LA_TURN_MS;
    float ox = (float)(cw - margin - iw - r.left), oy = (float)(ch - margin - ih - r.top);
    void *g = NULL;
    if (LA.from_hdc(m, &g) == 0 && g) {
        LA.smoothing(g, 4); LA.interp(g, 7); LA.pixoff(g, 4);     /* anti-alias, high-quality bicubic, half-pixel offset */
        LA.translate(g, ox, oy, 0); LA.scale(g, s, s, 0);        /* (order 0 = prepend: the last call applies first to the points) */
        LA.translate(g, LA_KONG_PIN_X, LA_KONG_PIN_Y, 0);
        LA.rotate(g, -360.0f * turn, 0);                         /* negative = counter-clockwise on screen */
        LA.translate(g, -LA_ARM_PIN_X, -LA_ARM_PIN_Y, 0);
        LA.draw(g, LA.arm, 0, 0, LA_ARM_W, LA_ARM_H);            /* the arm, behind */
        LA.reset(g);
        LA.translate(g, ox, oy, 0); LA.scale(g, s, s, 0);
        LA.draw(g, LA.back, 0, 0, LA_KONG_W, LA_KONG_H);         /* Kong's black silhouette over it, Kong on top */
        LA.draw(g, LA.kong, 0, 0, LA_KONG_W, LA_KONG_H);
        LA.del(g);
    }
    char note[sizeof g_la_note];
    AcquireSRWLockShared(&g_la_note_lock); memcpy(note, g_la_note, sizeof note); ReleaseSRWLockShared(&g_la_note_lock);
    if (note[0]) {
        HFONT of = (HFONT)SelectObject(m, LA.font);
        SetBkMode(m, TRANSPARENT); SetTextColor(m, RGB(200, 200, 200));
        RECT tr = {0, 0, (int)ox - fh / 2, hh - margin - ih / 6};
        DrawTextA(m, note, -1, &tr, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(m, of);
    }
    BitBlt(dc, r.left, r.top, w, hh, m, 0, 0, SRCCOPY);
    SelectObject(m, old); DeleteObject(bm); DeleteDC(m);
}

/* wndproc: WM_PAINT and the timer while the animation runs; returns 1 when it handled the message */
static int la_wndproc(HWND h, UINT msg, WPARAM w) {
    if (!LA.on) return 0;
    if (msg == WM_TIMER && w == LA_TIMER) {
        if (g_la_off) { KillTimer(h, LA_TIMER); LA.on = 0; return 1; }
        InvalidateRect(h, LA.last.right ? &LA.last : NULL, FALSE);      /* the bottom-right part painted last (a resize repaints it all) */
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        AcquireSRWLockExclusive(&g_la_lock);
        if (!g_la_off) la_paint(h, dc);
        ReleaseSRWLockExclusive(&g_la_lock);
        EndPaint(h, &ps);
        return 1;
    }
    return 0;
}

/* game's thread, at the first present (before any picture reaches the window): no GDI painting from here on. Callers
 * test g_la_first first, so this runs once (and the timing line is printed for hidden test windows too). */
static void la_stop(void) {
    AcquireSRWLockExclusive(&g_la_lock);   /* waits for a picture being painted */
    LONG was = InterlockedExchange(&g_la_off, 1);
    ReleaseSRWLockExclusive(&g_la_lock);
    if (!InterlockedExchange(&g_la_first, 1)) {
        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
        printf("display: the game's first present %.2f s after the process started, %.2f s after the window was made (the loading animation %s)\n", la_since_process_start(),
               g_la_t_window ? (double)(now.QuadPart - g_la_t_window) / (double)fq.QuadPart : 0.0, was ? "was not shown" : "stops here");
        fflush(stdout);
    }
}
