/* Mid-match refresh of mod files. Training reset is L+R+A (on an Xbox pad in the positional
 * layout, LB+RB and the right face button). That press still resets training. It also copies a
 * mod file the match already loaded, when the file on disk changed and stayed the same size:
 * the new bytes replace the old ones wherever they sit in the match's memory.
 *
 * The snapshot is taken at boot, from mods/, so it is the bytes the match will load. A later
 * export is what the press compares against. A file that still fits in the buffer the match
 * loaded is written over that buffer, and the leftover bytes are cleared. A file that grew past
 * that buffer cannot be swapped in place. A model or texture the graphics card already has does
 * not follow the copy in memory.
 */
#include "frame_hooks.h"
#include "kernel_watch.h"
#include "pad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SNAP_MAX = 4096, SNAP_FILE_MAX = 16 << 20, SCAN_MAX = 32, PATH_MAX_ = 600 };
enum { KIND_ANIM = 0, KIND_MODEL = 1 };

/* The console's A, L and R together: the same press as the training reset. */
#define RESET_COMBO ((1ull << 0) | (1ull << 6) | (1ull << 7))

typedef struct Snap {
    char *path;
    u8 *bytes;
    u32 size;       /* the bytes last written, which are what memory holds now */
    u32 capacity;   /* the buffer the match loaded: the file's size at boot */
    FILETIME mtime;
    int kind;
} Snap;

static Snap snaps[SNAP_MAX];
static int snap_count;
static volatile LONG snap_ready;
static volatile LONG snap_started;

static int ext_is(const char *name, const char *ext) {
    const char *d = strrchr(name, '.');
    return d && !_stricmp(d + 1, ext);
}

static int file_kind(const char *name) {
    if (ext_is(name, "nuanmb") || ext_is(name, "nuhlpb")) return KIND_ANIM;
    if (ext_is(name, "numdlb") || ext_is(name, "numshb") || ext_is(name, "nusktb") ||
        ext_is(name, "nutexb") || ext_is(name, "numatb")) return KIND_MODEL;
    return -1;
}

static u8 *read_all(const char *path, u32 *size, FILETIME *mtime) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return NULL;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return NULL;
    if (a.nFileSizeHigh || a.nFileSizeLow == 0 || a.nFileSizeLow > SNAP_FILE_MAX) return NULL;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    u8 *b = (u8 *)malloc(a.nFileSizeLow);
    DWORD got = 0;
    if (!b || !ReadFile(h, b, a.nFileSizeLow, &got, NULL) || got != a.nFileSizeLow) {
        free(b);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *size = got;
    if (mtime) *mtime = a.ftLastWriteTime;
    return b;
}

static void remember(const char *path, int kind) {
    if (snap_count >= SNAP_MAX) return;
    u32 size = 0;
    FILETIME mtime;
    u8 *b = read_all(path, &size, &mtime);
    if (!b) return;
    snaps[snap_count++] = (Snap){ _strdup(path), b, size, size, mtime, kind };
}

static void walk(const char *dir, int depth) {
    if (depth > 12 || snap_count >= SNAP_MAX) return;
    char pattern[PATH_MAX_];
    if ((size_t)snprintf(pattern, sizeof pattern, "%s\\*", dir) >= sizeof pattern) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
            continue;
        char child[PATH_MAX_];
        if ((size_t)snprintf(child, sizeof child, "%s\\%s", dir, fd.cFileName) >= sizeof child) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk(child, depth + 1);
            continue;
        }
        int kind = file_kind(fd.cFileName);
        if (kind >= 0) remember(child, kind);
    } while (FindNextFileA(h, &fd) && snap_count < SNAP_MAX);
    FindClose(h);
}

static DWORD WINAPI snap_main(void *unused) {
    (void)unused;
    char mods[PATH_MAX_], full[PATH_MAX_];
    snprintf(mods, sizeof mods, "%s\\..\\..\\mods", rt_data_dir ? rt_data_dir : ".");
    if (!GetFullPathNameA(mods, PATH_MAX_, full, NULL)) full[0] = 0;
    DWORD attr = full[0] ? GetFileAttributesA(full) : INVALID_FILE_ATTRIBUTES;
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) walk(full, 0);
    int anims = 0, models = 0;
    for (int i = 0; i < snap_count; i++) {
        if (snaps[i].kind == KIND_ANIM) anims++;
        else models++;
    }
    fprintf(stderr, "[mods] watching %d animation file%s and %d model file%s for a mid-match refresh on the training reset (L+R+A)\n",
            anims, anims == 1 ? "" : "s", models, models == 1 ? "" : "s");
    InterlockedExchange(&snap_ready, 1);
    return 0;
}

/* The old file's bytes inside one committed span. A new file that fits in `capacity` is written
 * there, and the rest of that buffer is cleared so the match does not keep the old tail. */
static u32 splice_span(u8 *base, size_t n, const u8 *oldb, u32 old_size, const u8 *neu, u32 new_size, u32 capacity, int write) {
    if (n < old_size || old_size < 8 || new_size == 0 || new_size > capacity) return 0;
    size_t at = old_size >= 0x50 ? 0x40 : 0;
    size_t sl = old_size - at >= 16 ? 16 : old_size - at;
    const u8 *sig = oldb + at;
    u32 hits = 0;
    size_t i = 0;
    while (i + old_size <= n) {
        size_t window = n - old_size - i + 1;
        u8 *hit = memchr(base + i + at, sig[0], window);
        if (!hit) break;
        size_t pos = (size_t)(hit - base) - at;
        size_t room = n - pos;
        if (pos + old_size <= n && memcmp(hit, sig, sl) == 0 && memcmp(base + pos, oldb, old_size) == 0) {
            if (write && new_size <= room) {
                memcpy(base + pos, neu, new_size);
                size_t zto = capacity < room ? capacity : room;
                if (zto > new_size) memset(base + pos + new_size, 0, zto - new_size);
                hits++;
            } else if (!write) {
                hits++;
            }
            size_t step = capacity < room ? capacity : room;
            if (step < old_size) step = old_size;
            i = pos + step;
        } else if (pos + 1 > i) {
            i = pos + 1;
        } else {
            break;
        }
    }
    return hits;
}

static int span_writable(const MEMORY_BASIC_INFORMATION *m) {
    if (m->State != MEM_COMMIT || (m->Protect & PAGE_GUARD)) return 0;
    DWORD p = m->Protect & 0xFF;
    return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
}

/* How many copies of oldb were overwritten. The kernel lock is held: nothing is mapped or
 * unmapped under the scan. */
static u32 splice_memory(const u8 *oldb, u32 old_size, const u8 *neu, u32 new_size, u32 capacity, int write) {
    static KernelRange ranges[4096];
    kernel_memory_hold();
    u32 n = kernel_writable_ranges(ranges, 4096);
    if (n > 4096) n = 4096;
    u32 hits = 0;
    for (u32 r = 0; r < n && hits < 8; r++) {
        if (ranges[r].hi <= ranges[r].lo) continue;
        u8 *p = (u8 *)G2H(ranges[r].lo);
        u8 *end = (u8 *)G2H(ranges[r].hi);
        while (p < end && hits < 8) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
            u8 *next = (u8 *)mbi.BaseAddress + mbi.RegionSize;
            if (next <= p) break;
            if (next > end) next = end;
            if (span_writable(&mbi)) {
                size_t len = (size_t)(next - p);
                hits += splice_span(p, len, oldb, old_size, neu, new_size, capacity, write);
            }
            p = next;
        }
    }
    kernel_memory_release();
    return hits;
}

static const char *short_name(const char *path) {
    const char *mods = strstr(path, "\\mods\\");
    if (!mods) mods = strstr(path, "/mods/");
    return mods ? mods + 6 : path;
}

static void refresh(void) {
    if (!snap_ready) {
        fprintf(stderr, "[mods] still reading the mods folder; press the training reset again in a moment\n");
        return;
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);
    int refreshed = 0, models = 0, grown = 0, missing = 0, already = 0;
    const char *names[3], *grown_names[3];
    int name_n = 0, grown_n = 0;
    int scanned = 0;
    for (int i = 0; i < snap_count; i++) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(snaps[i].path, GetFileExInfoStandard, &a)) continue;
        if (CompareFileTime(&a.ftLastWriteTime, &snaps[i].mtime) == 0) continue;
        u32 size = 0;
        FILETIME mtime;
        u8 *neu = read_all(snaps[i].path, &size, &mtime);
        if (!neu) continue;
        if (size == snaps[i].size && memcmp(neu, snaps[i].bytes, size) == 0) {
            snaps[i].mtime = mtime;
            free(neu);
            continue;
        }
        if (size > snaps[i].capacity) {
            grown++;
            if (grown_n < 3) grown_names[grown_n++] = short_name(snaps[i].path);
            free(neu);
            continue;
        }
        if (scanned >= SCAN_MAX) {
            free(neu);
            continue;
        }
        scanned++;
        u32 hits = splice_memory(snaps[i].bytes, snaps[i].size, neu, size, snaps[i].capacity, 1);
        if (!hits) {
            u32 has_new = splice_memory(neu, size, neu, size, size, 0);
            if (has_new) {
                free(snaps[i].bytes);
                snaps[i].bytes = neu;
                snaps[i].size = size;
                snaps[i].capacity = size;
                snaps[i].mtime = mtime;
                already++;
            } else {
                missing++;
                free(neu);
            }
            continue;
        }
        free(snaps[i].bytes);
        snaps[i].bytes = neu;
        snaps[i].size = size;
        snaps[i].mtime = mtime;
        refreshed++;
        if (snaps[i].kind == KIND_MODEL) models++;
        if (name_n < 3) names[name_n++] = short_name(snaps[i].path);
    }
    QueryPerformanceCounter(&t1);
    double ms = freq.QuadPart ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart : 0;
    if (!refreshed && !grown && !missing && !already) {
        fprintf(stderr, "[mods] training reset: no mod file has changed (%.0f ms)\n", ms);
        return;
    }
    fprintf(stderr, "[mods] training reset: refreshed %d file%s in %.0f ms", refreshed, refreshed == 1 ? "" : "s", ms);
    for (int n = 0; n < name_n; n++) fprintf(stderr, "%s%s", n ? ", " : " (", names[n]);
    if (name_n) fprintf(stderr, "%s)", refreshed > name_n ? ", ..." : "");
    fprintf(stderr, "\n");
    if (grown) {
        fprintf(stderr, "[mods] %d file%s grew past the buffer this match loaded, so it keeps the old one until you leave and rejoin",
                grown, grown == 1 ? "" : "s");
        for (int n = 0; n < grown_n; n++) fprintf(stderr, "%s%s", n ? ", " : " (", grown_names[n]);
        if (grown_n) fprintf(stderr, "%s)", grown > grown_n ? ", ..." : "");
        fprintf(stderr, "\n");
    }
    if (missing)
        fprintf(stderr, "[mods] %d changed file%s %s not in this match's memory (it has not been loaded, or the match copied it apart)\n",
                missing, missing == 1 ? "" : "s", missing == 1 ? "is" : "are");
    if (already)
        fprintf(stderr, "[mods] %d file%s already the new bytes in this match\n", already, already == 1 ? "" : "s");
    if (models)
        fprintf(stderr, "[mods] %d of those %s a model or texture; one the graphics card already has stays as it was until the fighter loads again\n",
                models, models == 1 ? "is" : "are");
    static int said_online;
    if (refreshed && !said_online) {
        said_online = 1;
        fprintf(stderr, "[mods] this changes your game only; the other side of an online match keeps the old file\n");
    }
}

static void mod_refresh_start(void) {
    if (InterlockedCompareExchange(&snap_started, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, snap_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* Once a frame from the first rendered frame, so the snapshot is the bytes on disk before a match. */
static void mod_refresh_boot(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    mod_refresh_start();
}

/* Before the fighter's frame, so the training reset that this same press runs reads the new bytes. */
static void mod_refresh_before(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    mod_refresh_start();
    if (menu_blocks_input()) return;
    static u64 prev[PAD_SLOTS];
    static LONG busy;
    int fire = 0;
    for (u32 s = 0; s < PAD_SLOTS; s++) {
        PadState st;
        if (!pad_get(s, &st)) { prev[s] = 0; continue; }
        if ((st.buttons & RESET_COMBO) == RESET_COMBO && (prev[s] & RESET_COMBO) != RESET_COMBO) fire = 1;
        prev[s] = st.buttons;
    }
    if (fire && InterlockedCompareExchange(&busy, 1, 0) == 0) {
        refresh();
        InterlockedExchange(&busy, 0);
    }
}

__attribute__((constructor)) static void mod_refresh_register(void) {
    frame_hook_add(FRAME_HOOK_END, 300, NULL, mod_refresh_boot, NULL);
    frame_hook_add(FRAME_HOOK_LINE, 300, mod_refresh_before, NULL, NULL);
}
