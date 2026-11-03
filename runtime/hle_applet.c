/* Library applets - the system's own full-screen programs a game can call up: web pages
 * (the electronic manual, news), the software keyboard, the shop. The keyboard is a text box
 * here (Windows' dialog, an alert on macOS, zenity's on Linux); the others don't exist, so each
 * behaves as if the player opened it and immediately backed out. */
#define _CRT_SECURE_NO_WARNINGS
#include "host.h"
#include "gpu_backend.h"

void watchdog_pause(int on);   /* debug_watchdog.c */
int menu_osk_run(const char *title, const char *initial, int max_len, char *out, size_t cap);   /* gpu_menu.c */

#define R_CANCELED RESULT(162, 1)         /* "the user closed the applet" */
#define R_SWKBD_CANCELED RESULT(159, 1)   /* nn::swkbd's own: what ShowKeyboard returns for "back" */

/* Web pages: return at once, as if closed with the B button. */
HLE(_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE) { c->x[0] = 0; }
HLE(_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE) { c->x[0] = 0; }
HLE(_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE) { c->x[0] = 0; }
HLE(_ZN2nn3web16ShowYouTubeVideoEPNS0_18WebPageReturnValueERKNS0_19ShowYouTubeVideoArgE) { c->x[0] = 0; }

/* ---- the software keyboard: a text box --------------------------------------------- */
/* ShowKeyboard(String *out, const ShowKeyboardArg *arg). The argument starts with the
 * KeyboardConfig: header text (u16[65]) at 0x24, sub text (u16[129]) at 0xA6, guide text
 * (u16[257]) at 0x1A8, maximum and minimum length at 0x3AC/0x3B0, "new lines allowed" and
 * "answer in UTF-8" at 0x3BC/0x3BD; the initial text lies in the work buffer (pointer at
 * 0x4D0) at the offset and length (in characters) at 0x3C0/0x3C4. The answer goes to
 * out = {pointer, size in bytes}, NUL-terminated; 0 means OK, R_SWKBD_CANCELED backed out. */
#ifdef _WIN32
enum { ID_TEXT = 100, ID_EDIT = 101 };

typedef struct Keyboard {
    WCHAR header[66], sub[130], guide[258], text[1024];
    int max_len, min_len, multiline;
} Keyboard;

static void guest_text(WCHAR *dst, size_t cap, u64 src, size_t max) {
    size_t n = 0;
    while (n + 1 < cap && n < max && ld16(src + 2 * n)) { dst[n] = (WCHAR)ld16(src + 2 * n); n++; }
    dst[n] = 0;
}

static INT_PTR CALLBACK keyboard_proc(HWND dlg, UINT msg, WPARAM w, LPARAM l) {
    Keyboard *k = (Keyboard *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        k = (Keyboard *)l;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)k);
        WCHAR prompt[600] = L"";   /* header, sub text, then the guide (the box's placeholder on the console) */
        if (k->header[0]) wcscat(prompt, k->header);
        if (k->sub[0]) { if (prompt[0]) wcscat(prompt, L"\n"); wcscat(prompt, k->sub); }
        if (k->guide[0]) {
            int bracket = prompt[0] != 0;
            if (bracket) wcscat(prompt, L"\n(");
            wcscat(prompt, k->guide);
            if (bracket) wcscat(prompt, L")");
        }
        SetDlgItemTextW(dlg, ID_TEXT, prompt[0] ? prompt : L"Enter text");
        SendDlgItemMessageW(dlg, ID_EDIT, EM_LIMITTEXT, (WPARAM)k->max_len, 0);
        SetDlgItemTextW(dlg, ID_EDIT, k->text);
        SendDlgItemMessageW(dlg, ID_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, ID_EDIT));
        return FALSE;   /* (focus set) */
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            GetDlgItemTextW(dlg, ID_EDIT, k->text, sizeof k->text / sizeof *k->text);
            if ((int)wcslen(k->text) < k->min_len) { MessageBeep(MB_ICONWARNING); return TRUE; }
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    }
    (void)l;
    return FALSE;
}

/* An in-memory dialog template: a prompt, a text box, OK and Cancel. */
static WORD *put_words(WORD *p, const WCHAR *s) { do *p++ = *s; while (*s++); return p; }
static WORD *put_item(WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const WCHAR *text) {
    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);   /* items are DWORD-aligned */
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;   /* a predefined class: 0x80 button, 0x81 edit, 0x82 static */
    p = put_words(p, text);
    *p++ = 0;                    /* no creation data */
    return p;
}

static int ask_text(Keyboard *k) {
    static WORD buf[1024];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    t->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cdit = 4;
    t->cx = 300; t->cy = k->multiline ? 140 : 92;
    WORD *p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;                           /* no menu, the default class */
    p = put_words(p, L"Super Smash Bros. Ultimate - keyboard");
    *p++ = 10;                                    /* font size */
    p = put_words(p, L"Segoe UI");
    short edit_h = k->multiline ? 60 : 14, y = 44;
    p = put_item(p, SS_LEFT, 8, 6, 284, 34, ID_TEXT, 0x82, L"");
    p = put_item(p, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (k->multiline ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN : 0),
                 8, y, 284, edit_h, ID_EDIT, 0x81, L"");
    y = (short)(y + edit_h + 10);
    p = put_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 186, y, 50, 14, IDOK, 0x80, L"OK");
    put_item(p, BS_PUSHBUTTON | WS_TABSTOP, 242, y, 50, 14, IDCANCEL, 0x80, L"Cancel");
    watchdog_pause(1);   /* (the game waits for the dialog: no frames meanwhile) */
    int ok = DialogBoxIndirectParamW(GetModuleHandleW(NULL), t, (HWND)gfx_window_handle(), keyboard_proc, (LPARAM)k) == 1;
    watchdog_pause(0);
    return ok;
}

HLE(_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE) {
    u64 out = c->x[0], arg = c->x[1];
    static Keyboard k;
    memset(&k, 0, sizeof k);
    guest_text(k.header, 66, arg + 0x24, 64);
    guest_text(k.sub, 130, arg + 0xA6, 128);
    guest_text(k.guide, 258, arg + 0x1A8, 256);
    k.max_len = (int)ld32(arg + 0x3AC);
    k.min_len = (int)ld32(arg + 0x3B0);
    if (k.max_len <= 0 || k.max_len > 1000) k.max_len = 1000;
    if (k.min_len < 0 || k.min_len > k.max_len) k.min_len = 0;
    k.multiline = ld8(arg + 0x3BC) != 0;
    int utf8 = ld8(arg + 0x3BD) != 0;
    u64 work = ld64(arg + 0x4D0);
    u32 offset = ld32(arg + 0x3C0), length = ld32(arg + 0x3C4);
    if (work && length) guest_text(k.text, 1024, work + offset, length < 1000 ? length : 1000);
    /* the menu's text grid over the game (pad or keyboard, as the console's own keyboard), unless
     * SSBU_SWKBD=dialog or the renderer can't show a frame again; letters, digits and - _ . : only */
    int ok = -1;
    const char *how = getenv("SSBU_SWKBD");
    if (!(how && !_stricmp(how, "dialog")) && !k.multiline) {
        char title[200], initial[64], typed[64];
        WideCharToMultiByte(CP_UTF8, 0, k.header[0] ? k.header : k.guide, -1, title, sizeof title, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, k.text, -1, initial, sizeof initial, NULL, NULL);
        watchdog_pause(1);
        ok = menu_osk_run(title, initial, k.max_len, typed, sizeof typed);
        watchdog_pause(0);
        if (ok == 1) MultiByteToWideChar(CP_UTF8, 0, typed, -1, k.text, 1024);
    }
    if (ok < 0) ok = ask_text(&k);
    /* the text (also on Cancel: what the box held, as the console's keyboard reports it) */
    u64 ptr = out ? ld64(out) : 0, size = out ? ld64(out + 8) : 0;
    if (ptr && size >= (utf8 ? 1u : 2u)) {
        if (utf8) {
            char buf[4096];
            int n = WideCharToMultiByte(CP_UTF8, 0, k.text, -1, buf, sizeof buf, NULL, NULL);
            if (n <= 0) { buf[0] = 0; n = 1; }
            if ((u64)n > size) { n = (int)size; buf[n - 1] = 0; }
            memcpy(P(void, ptr), buf, (size_t)n);
        } else {
            size_t n = wcslen(k.text), room = (size_t)(size / 2) - 1;
            if (n > room) n = room;
            for (size_t i = 0; i < n; i++) st16(ptr + 2 * i, k.text[i]);
            st16(ptr + 2 * n, 0);
        }
    }
    c->x[0] = ok ? 0 : R_SWKBD_CANCELED;
}

#else
/* Linux: the text box is zenity's (GNOME's dialog tool), else kdialog's, else the terminal's.
 * macOS: an alert over the game's window (macos/window.m: AppKit's, on the main thread; it shows
 * in the game's full-screen space too, and takes the keyboard), else the terminal's. */
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static size_t utf8_of(const u16 *w, size_t n, char *out, size_t cap) {   /* UTF-16 -> UTF-8 */
    size_t o = 0;
    for (size_t i = 0; i < n && w[i]; i++) {
        u32 c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
        u8 b[4];
        int k = c < 0x80 ? (b[0] = (u8)c, 1) : c < 0x800 ? (b[0] = (u8)(0xC0 | c >> 6), b[1] = (u8)(0x80 | (c & 63)), 2)
              : c < 0x10000 ? (b[0] = (u8)(0xE0 | c >> 12), b[1] = (u8)(0x80 | (c >> 6 & 63)), b[2] = (u8)(0x80 | (c & 63)), 3)
              : (b[0] = (u8)(0xF0 | c >> 18), b[1] = (u8)(0x80 | (c >> 12 & 63)), b[2] = (u8)(0x80 | (c >> 6 & 63)), b[3] = (u8)(0x80 | (c & 63)), 4);
        if (o + (size_t)k + 1 > cap) break;
        memcpy(out + o, b, (size_t)k);
        o += (size_t)k;
    }
    out[o] = 0;
    return o;
}

static size_t utf16_of(const char *s, u16 *out, size_t cap) {   /* UTF-8 -> UTF-16 */
    size_t o = 0;
    const u8 *p = (const u8 *)s;
    while (*p && o + 1 < cap) {
        u32 c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        while (more-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 63);
        if (c >= 0x10000) {
            if (o + 2 >= cap) break;
            c -= 0x10000;
            out[o++] = (u16)(0xD800 + (c >> 10));
            out[o++] = (u16)(0xDC00 + (c & 0x3FF));
        } else out[o++] = (u16)c;
    }
    out[o] = 0;
    return o;
}

static size_t count16(const u16 *w) { size_t n = 0; while (w[n]) n++; return n; }

#ifdef __APPLE__
/* (macos/window.m) 1 OK (the answer in text), 0 cancelled, -1 no window */
int rt_window_ask_text(const char *prompt, const char *initial, char *text, size_t cap);
#else
/* Run a dialog program; its standard output (trailing newline dropped) into out. Returns its
 * exit status, or -1 if it could not run. */
static int run_dialog(char *const argv[], char *out, size_t cap) {
    int fds[2];
    if (pipe(fds)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (err) { close(fds[0]); return -1; }
    size_t n = 0;
    for (ssize_t r; n + 1 < cap && (r = read(fds[0], out + n, cap - 1 - n)) != 0;) {
        if (r < 0) { if (errno == EINTR) continue; break; }
        n += (size_t)r;
    }
    out[n] = 0;
    close(fds[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (n && out[n - 1] == '\n') out[--n] = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

/* Ask for the text: 1 OK (answer in text), 0 cancelled. */
static int ask_text_now(const char *prompt, const char *initial, int multiline, char *text, size_t cap) {
#ifdef __APPLE__
    (void)multiline;   /* (one line, as on Linux) */
    int r = rt_window_ask_text(prompt, initial, text, cap);
    if (r >= 0) return r;
#else
    char marked[4096];   /* (zenity reads its text as Pango markup) */
    size_t m = 0;
    for (const char *q = prompt; *q && m + 6 < sizeof marked; q++) {
        const char *e = *q == '&' ? "&amp;" : *q == '<' ? "&lt;" : *q == '>' ? "&gt;" : NULL;
        if (e) { memcpy(marked + m, e, strlen(e)); m += strlen(e); } else marked[m++] = *q;
    }
    marked[m] = 0;
    char *zenity_line[] = {"zenity", "--entry", "--title=Super Smash Bros. Ultimate - keyboard", "--width=480", "--text", marked,
                           "--entry-text", (char *)initial, NULL};
    char *kdialog[] = {"kdialog", "--title", "Super Smash Bros. Ultimate - keyboard", "--inputbox", (char *)prompt, (char *)initial, NULL};
    (void)multiline;   /* (the answer is one line: new lines are rare in what the game asks for) */
    int r = run_dialog(zenity_line, text, cap);
    if (r == 0) return 1;
    if (r == 1) return 0;   /* Cancel */
    r = run_dialog(kdialog, text, cap);
    if (r == 0) return 1;
    if (r == 1) return 0;
#endif
    if (isatty(0)) {   /* no dialog program: the terminal the game runs in */
        fprintf(stderr, "\n[keyboard] %s\n[keyboard] text (empty line keeps \"%s\"): ", prompt, initial);
        if (!fgets(text, (int)cap, stdin)) return 0;
        text[strcspn(text, "\r\n")] = 0;
        if (!text[0]) snprintf(text, cap, "%s", initial);
        return 1;
    }
#ifdef __APPLE__
    fprintf(stderr, "[keyboard] no window or terminal: answering with the initial text\n");
#else
    fprintf(stderr, "[keyboard] no zenity, kdialog or terminal: answering with the initial text\n");
#endif
    snprintf(text, cap, "%s", initial);
    return 1;
}

static int ask_text(const char *prompt, const char *initial, int multiline, char *text, size_t cap) {
    watchdog_pause(1);   /* (the game waits for the dialog: no frames meanwhile) */
    int ok = ask_text_now(prompt, initial, multiline, text, cap);
    watchdog_pause(0);
    return ok;
}

HLE(_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE) {
    u64 out = c->x[0], arg = c->x[1];
    static u16 header[66], sub[130], guide[258], initial[1024], answer[1024];
    u16 *texts[3] = {header, sub, guide};
    const u64 at[3] = {0x24, 0xA6, 0x1A8};
    const size_t max[3] = {64, 128, 256};
    for (int t = 0; t < 3; t++) {
        size_t n = 0;
        while (n < max[t] && ld16(arg + at[t] + 2 * n)) { texts[t][n] = ld16(arg + at[t] + 2 * n); n++; }
        texts[t][n] = 0;
    }
    int max_len = (int)ld32(arg + 0x3AC), min_len = (int)ld32(arg + 0x3B0);
    if (max_len <= 0 || max_len > 1000) max_len = 1000;
    if (min_len < 0 || min_len > max_len) min_len = 0;
    int multiline = ld8(arg + 0x3BC) != 0, utf8 = ld8(arg + 0x3BD) != 0;
    u64 work = ld64(arg + 0x4D0);
    u32 offset = ld32(arg + 0x3C0), length = ld32(arg + 0x3C4);
    size_t ni = 0;
    if (work && length) for (; ni < length && ni < 1000 && ld16(work + offset + 2 * ni); ni++) initial[ni] = ld16(work + offset + 2 * ni);
    initial[ni] = 0;
    /* header, sub text, then the guide (the box's placeholder on the console) */
    char prompt[2048] = "", part[1024], init8[4096], text8[4096];
    for (int t = 0; t < 3; t++) {
        if (!texts[t][0]) continue;
        utf8_of(texts[t], count16(texts[t]), part, sizeof part);
        size_t l = strlen(prompt);
        snprintf(prompt + l, sizeof prompt - l, t == 2 && l ? "\n(%s)" : l ? "\n%s" : "%s", part);
    }
    if (!prompt[0]) snprintf(prompt, sizeof prompt, "Enter text");
    utf8_of(initial, ni, init8, sizeof init8);
    int ok;
    size_t n;
    for (;;) {
        ok = ask_text(prompt, init8, multiline, text8, sizeof text8);
        n = utf16_of(ok ? text8 : init8, answer, sizeof answer / sizeof *answer);
        if ((int)n > max_len) answer[n = (size_t)max_len] = 0;
        if (!ok || (int)n >= min_len) break;
        snprintf(init8, sizeof init8, "%s", text8);   /* too short: ask again */
    }
    u64 ptr = out ? ld64(out) : 0, size = out ? ld64(out + 8) : 0;
    if (ptr && size >= (utf8 ? 1u : 2u)) {
        if (utf8) {
            char buf[4096];
            size_t m = utf8_of(answer, n, buf, sizeof buf) + 1;
            if (m > size) { m = (size_t)size; buf[m - 1] = 0; }
            memcpy(P(void, ptr), buf, m);
        } else {
            size_t room = (size_t)(size / 2) - 1, k = n < room ? n : room;
            for (size_t i = 0; i < k; i++) st16(ptr + 2 * i, answer[i]);
            st16(ptr + 2 * k, 0);
        }
    }
    c->x[0] = ok ? 0 : R_SWKBD_CANCELED;
}
#endif

/* The shop: unavailable offline. */
HLE(_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKcRKNS_7account10UserHandleE) { c->x[0] = R_CANCELED; }
HLE(_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidERKNS_7account10UserHandleE) { c->x[0] = R_CANCELED; }
HLE(_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdE) { c->x[0] = R_CANCELED; }
HLE(_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE) { c->x[0] = R_CANCELED; }

/* A friend's profile page. */
HLE(_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_) { c->x[0] = 0; }
